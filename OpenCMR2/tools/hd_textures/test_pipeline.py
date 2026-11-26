#!/usr/bin/env python3
"""Unit test suite for the OpenCMR2 Stage 9 HD Texture Pipeline toolkit.

Covers:
  - Synthetic CMR2 .BFL archive construction & read-only parsing (gzip & raw CMPR,
    4-byte filename alignment padding, exact directory offset at EOF)
  - DDS decoding: DXT1/BC1 (opaque + 1-bit alpha), DXT3/BC2, DXT5/BC3,
    and uncompressed 32-bit BGRA8 / 24-bit BGR8
  - TGA decoding: 24-bit BGR bottom-up, 32-bit BGRA bottom-up, top-down flag
  - PNG encode/decode round-trip
  - Canonical texture key normalization (matching game/Graphics.cpp & vector_sprite.cpp)
  - Category classification across all 6 categories + OpenCMR2 vectorized UI skip list
  - Seamless wrap-around tiling (zero border seam discontinuity on albedo & normal map)
  - Alpha cutout halo prevention (no dark/black color fringe on semi-transparent edges)
  - Normal map generation (flat -> (128,128,255), gradient -> tilted tangent normal)
  - Specular/reflection map generation (category contrast + A==255)
  - End-to-end `run_pipeline.py` CLI workflow (--dry-run, --categories, --limit, --workers, --manifest)
"""

from __future__ import annotations

import gzip
import json
import math
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from typing import Dict, List, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _image_io import RGBAImage, decode_png, encode_png, load_png
from bfl_extract import (
    BFL_MAGIC,
    BFLArchive,
    decode_dds,
    decode_tga,
    normalize_texture_key,
    parse_bfl_bytes,
)
from classify_textures import classify_texture
from run_pipeline import run_pipeline
from upscale_hd import (
    compute_target_size,
    dilate_alpha_rgb,
    generate_normal_map,
    generate_specular_map,
    lanczos_resize,
    process_texture,
)


# ---------------------------------------------------------------------------
# Synthetic binary fixture builders (BFL, DDS, TGA)
# ---------------------------------------------------------------------------


def build_synthetic_bfl(files: List[Tuple[str, bytes]], compress_gzip: bool = True) -> bytes:
    """Build a valid CMR2 .BFL binary buffer matching game/BFL.h & GenericFileLoader.cpp."""
    blobs = bytearray()
    directory = bytearray()

    for name, data in files:
        offset = len(blobs)
        blobs.extend(data)
        name_bytes = name.lower().encode("latin-1")
        name_len = len(name_bytes)
        padded_len = (name_len + 3) & ~3
        directory.extend(struct.pack("<IiI", len(data), offset, name_len))
        directory.extend(name_bytes)
        directory.extend(b"\x00" * (padded_len - name_len))

    dir_offset = len(blobs)
    payload = bytes(blobs) + bytes(directory) + struct.pack("<i", dir_offset)
    header = BFL_MAGIC + struct.pack("<I", len(payload))
    raw_bfl = header + payload
    return gzip.compress(raw_bfl) if compress_gzip else raw_bfl


def _rgb888_to_rgb565(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def build_dds_header(
    width: int,
    height: int,
    fourcc: bytes = b"\x00\x00\x00\x00",
    pf_flags: int = 0x41,
    bit_count: int = 32,
    masks: Tuple[int, int, int, int] = (0x00FF0000, 0x0000FF00, 0x000000FF, 0xFF000000),
    linear_size: int = 0,
) -> bytes:
    """Build a 128-byte DirectDraw Surface (DDS) file header matching game/Texture.h."""
    dw_flags = 0x1 | 0x2 | 0x4 | 0x1000 | (0x00080000 if linear_size else 0)
    header_part1 = struct.pack(
        "<IIIIIII",
        124,  # dwSize
        dw_flags,
        height,
        width,
        linear_size,
        0,  # dwBackBufferCount
        1,  # dwMipMapCount
    )
    reserved = b"\x00" * (44)  # dwAlphaBitDepth..colourKeys[8] (11 DWORDs = 44 bytes)
    pixel_format = struct.pack(
        "<II4sIIIII",
        32,  # ddpf.dwSize
        pf_flags,
        fourcc,
        bit_count,
        masks[0],
        masks[1],
        masks[2],
        masks[3],
    )
    caps_and_stage = b"\x00" * 20  # ddsCaps[4] + dwTextureStage
    hdr = b"DDS " + header_part1 + reserved + pixel_format + caps_and_stage
    assert len(hdr) == 128
    return hdr


def build_dds_dxt1_4x4(c0_rgb: Tuple[int, int, int], c1_rgb: Tuple[int, int, int], indices: List[int]) -> bytes:
    """Build a 4x4 DXT1 DDS file from two RGB endpoints and 16 2-bit indices."""
    c0 = _rgb888_to_rgb565(*c0_rgb)
    c1 = _rgb888_to_rgb565(*c1_rgb)
    bits = 0
    for i, idx in enumerate(indices):
        bits |= (idx & 0x3) << (i * 2)
    block = struct.pack("<HHI", c0, c1, bits)
    return build_dds_header(4, 4, fourcc=b"DXT1", pf_flags=0x4, bit_count=0, masks=(0, 0, 0, 0), linear_size=8) + block


def build_dds_dxt3_4x4(c0_rgb: Tuple[int, int, int], c1_rgb: Tuple[int, int, int], alphas4: List[int]) -> bytes:
    c0 = _rgb888_to_rgb565(*c0_rgb)
    c1 = _rgb888_to_rgb565(*c1_rgb)
    if c0 <= c1:
        c0, c1 = c1, c0
    alpha_bits = 0
    for i, a4 in enumerate(alphas4):
        alpha_bits |= (a4 & 0xF) << (i * 4)
    block = struct.pack("<QHHI", alpha_bits, c0, c1, 0)
    return build_dds_header(4, 4, fourcc=b"DXT3", pf_flags=0x4, bit_count=0, masks=(0, 0, 0, 0), linear_size=16) + block


def build_dds_dxt5_4x4(
    c0_rgb: Tuple[int, int, int],
    c1_rgb: Tuple[int, int, int],
    a0: int,
    a1: int,
    alpha_indices: List[int],
) -> bytes:
    c0 = _rgb888_to_rgb565(*c0_rgb)
    c1 = _rgb888_to_rgb565(*c1_rgb)
    alpha_bits = 0
    for i, idx in enumerate(alpha_indices):
        alpha_bits |= (idx & 0x7) << (i * 3)
    block = bytes((a0, a1)) + alpha_bits.to_bytes(6, "little") + struct.pack("<HHI", c0, c1, 0)
    return build_dds_header(4, 4, fourcc=b"DXT5", pf_flags=0x4, bit_count=0, masks=(0, 0, 0, 0), linear_size=16) + block


def build_dds_bgra32(image: RGBAImage) -> bytes:
    bgra = bytearray(image.width * image.height * 4)
    for i in range(0, len(image.data), 4):
        bgra[i] = image.data[i + 2]
        bgra[i + 1] = image.data[i + 1]
        bgra[i + 2] = image.data[i]
        bgra[i + 3] = image.data[i + 3]
    return build_dds_header(image.width, image.height, pf_flags=0x41, bit_count=32) + bytes(bgra)


def build_tga(image: RGBAImage, bpp: int = 32, top_down: bool = False) -> bytes:
    desc = (8 if bpp == 32 else 0) | (0x20 if top_down else 0)
    header = struct.pack("<BBBHHBHHHHBB", 0, 0, 2, 0, 0, 0, 0, 0, image.width, image.height, bpp, desc)
    body = bytearray()
    for row_i in range(image.height):
        y = row_i if top_down else (image.height - 1 - row_i)
        for x in range(image.width):
            r, g, b, a = image.get_pixel(x, y)
            body.extend((b, g, r) if bpp == 24 else (b, g, r, a))
    return header + bytes(body)


# ---------------------------------------------------------------------------
# Unit tests
# ---------------------------------------------------------------------------


class TestBFLAndCodecs(unittest.TestCase):
    def test_bfl_gzip_and_uncompressed_parsing_with_padding(self):
        # Include 1-, 2-, 3-, and 4-byte-modulo filename lengths to verify 4-byte alignment
        img = RGBAImage.new(4, 4, (200, 100, 50, 255))
        dds_blob = build_dds_bgra32(img)
        tga_blob = build_tga(img, bpp=32)
        entries_in = [
            ("a.dds", dds_blob),            # len 5 -> pad 3
            ("ab.tga", tga_blob),           # len 6 -> pad 2
            ("abc.dds", dds_blob),          # len 7 -> pad 1
            ("road01.tga", tga_blob),       # len 10 -> pad 2
            ("fordbodf.dds", dds_blob),     # len 12 -> pad 0
            ("notes.txt", b"not a texture"),
        ]
        for gz in (True, False):
            raw_bfl = build_synthetic_bfl(entries_in, compress_gzip=gz)
            parsed, payload = parse_bfl_bytes(raw_bfl)
            self.assertEqual(len(parsed), len(entries_in))
            textures = [e for e in parsed if e.is_texture]
            self.assertEqual(len(textures), 5)
            for (expected_name, expected_data), entry in zip(entries_in, parsed):
                self.assertEqual(entry.name, expected_name)
                self.assertEqual(entry.size, len(expected_data))
                self.assertEqual(payload[entry.offset : entry.offset + entry.size], expected_data)

    def test_bfl_archive_file_readonly(self):
        img = RGBAImage.new(4, 4, (10, 20, 30, 255))
        bfl_bytes = build_synthetic_bfl([("uk_tarmac01.dds", build_dds_bgra32(img))], compress_gzip=True)
        with tempfile.TemporaryDirectory() as td:
            p = Path(td) / "stage1.bfl"
            p.write_bytes(bfl_bytes)
            before = p.read_bytes()
            arch = BFLArchive(p)
            self.assertEqual(len(arch.list_entries(textures_only=True)), 1)
            decoded = decode_dds(arch.read_entry("uk_tarmac01.dds"))
            self.assertEqual(decoded.get_pixel(2, 2), (10, 20, 30, 255))
            self.assertEqual(p.read_bytes(), before)

    def test_dds_dxt1_opaque_and_transparent(self):
        # Opaque mode: c0 > c1 (white vs black)
        dds_opaque = build_dds_dxt1_4x4((255, 255, 255), (0, 0, 0), [0, 1, 2, 3] * 4)
        img = decode_dds(dds_opaque)
        self.assertEqual(img.get_pixel(0, 0), (255, 255, 255, 255))
        self.assertEqual(img.get_pixel(1, 0), (0, 0, 0, 255))
        self.assertEqual(img.get_pixel(2, 0)[3], 255)
        self.assertGreater(img.get_pixel(2, 0)[0], img.get_pixel(3, 0)[0])

        # 1-bit alpha mode: c0 <= c1 (index 3 is transparent black)
        dds_alpha = build_dds_dxt1_4x4((0, 0, 0), (255, 255, 255), [0, 1, 2, 3] * 4)
        img_a = decode_dds(dds_alpha)
        self.assertEqual(img_a.get_pixel(3, 0), (0, 0, 0, 0))
        self.assertEqual(img_a.get_pixel(2, 0), (127, 127, 127, 255))

    def test_dds_dxt3_and_dxt5(self):
        alphas4 = [0, 5, 10, 15] * 4
        img3 = decode_dds(build_dds_dxt3_4x4((248, 0, 0), (0, 0, 248), alphas4))
        self.assertEqual(img3.get_pixel(0, 0)[3], 0)
        self.assertEqual(img3.get_pixel(1, 0)[3], 0x55)
        self.assertEqual(img3.get_pixel(2, 0)[3], 0xAA)
        self.assertEqual(img3.get_pixel(3, 0)[3], 0xFF)

        # DXT5 with a0=255, a1=0 -> 6 interpolated alphas
        alpha_idx = [0, 1, 2, 7] * 4
        img5 = decode_dds(build_dds_dxt5_4x4((0, 252, 0), (0, 0, 0), 255, 0, alpha_idx))
        self.assertEqual(img5.get_pixel(0, 0)[3], 255)
        self.assertEqual(img5.get_pixel(1, 0)[3], 0)
        self.assertAlmostEqual(img5.get_pixel(2, 0)[3], (6 * 255) // 7, delta=1)
        self.assertAlmostEqual(img5.get_pixel(3, 0)[3], (1 * 255) // 7, delta=1)

    def test_dds_uncompressed_and_tga_and_png_roundtrip(self):
        img = RGBAImage.new(6, 4, (0, 0, 0, 255))
        img.set_pixel(0, 0, (255, 10, 20, 128))
        img.set_pixel(5, 3, (12, 234, 56, 255))

        dds_dec = decode_dds(build_dds_bgra32(img))
        self.assertEqual(dds_dec.get_pixel(0, 0), (255, 10, 20, 128))
        self.assertEqual(dds_dec.get_pixel(5, 3), (12, 234, 56, 255))

        for top_down in (False, True):
            tga32 = decode_tga(build_tga(img, bpp=32, top_down=top_down))
            self.assertEqual(tga32.get_pixel(0, 0), (255, 10, 20, 128))
            self.assertEqual(tga32.get_pixel(5, 3), (12, 234, 56, 255))

        tga24 = decode_tga(build_tga(img, bpp=24, top_down=False))
        self.assertEqual(tga24.get_pixel(0, 0), (255, 10, 20, 255))
        self.assertEqual(tga24.get_pixel(5, 3), (12, 234, 56, 255))

        png_dec = decode_png(encode_png(img))
        self.assertEqual(png_dec.data, img.data)

    def test_normalize_texture_key(self):
        self.assertEqual(normalize_texture_key(r"\NEWIMAGE\OSD\CODRIV\ARROWS4.TGA"), "arrows4")
        self.assertEqual(normalize_texture_key(r"Cars\Ford\FORDBODF.DDS"), "fordbodf")
        self.assertEqual(normalize_texture_key("tracks/uk/UK_Road_01.tga"), "uk_road_01")


class TestClassification(unittest.TestCase):
    def test_vectorized_ui_and_fonts_are_skipped(self):
        ui_names = [
            "hel_13pt.tga",
            "HEL_BOLD.TGA",
            "lcd_640.tga",
            "dot.tga",
            "dot01.tga",
            "Ar_640A.tga",
            "Ar_640D.tga",
            "SmMatrix.tga",
            "ARROWS4.TGA",
            "cha01.tga",
            "cha08.tga",
            "chalblob.tga",
            "Tick.tga",
            "Cross.tga",
            "Box.tga",
            "Circle.tga",
            "rev_640.tga",
            "speeddigit.dds",
            "carrevct.dds",
        ]
        for name in ui_names:
            res = classify_texture(name)
            self.assertEqual(res.category, "ui_skip", f"Expected ui_skip for {name}, got {res}")

    def test_world_categories_by_name_and_pixels(self):
        self.assertEqual(classify_texture("uk_tarmac01.dds").category, "road_surface")
        self.assertEqual(classify_texture("gravel_wet.tga").category, "road_surface")
        self.assertEqual(classify_texture("fordbodf.dds", "data/cars/focusc5.bfl").category, "car_livery")
        self.assertEqual(classify_texture("subaru_wheel.dds").category, "car_livery")
        self.assertEqual(classify_texture("pine_tree01.tga").category, "foliage_alpha")
        self.assertEqual(classify_texture("crowd_fence.dds").category, "foliage_alpha")
        self.assertEqual(classify_texture("stone_wall02.dds").category, "buildings_props")
        self.assertEqual(classify_texture("sponsor_banner.tga").category, "buildings_props")
        self.assertEqual(classify_texture("uk_sky_clouds.dds").category, "sky_backdrop")

        # Unknown name with alpha cutout pixels should classify as foliage_alpha
        cutout = RGBAImage.new(16, 16, (0, 0, 0, 0))
        for y in range(4, 12):
            for x in range(4, 12):
                cutout.set_pixel(x, y, (40, 150, 45, 255))
        res_cut = classify_texture("prop_mystery01.dds", image=cutout)
        self.assertEqual(res_cut.category, "foliage_alpha")
        self.assertTrue(res_cut.has_alpha_cutout)


class TestHDProcessing(unittest.TestCase):
    def test_compute_target_size_cap_and_aspect(self):
        self.assertEqual(compute_target_size(128, 128), (512, 512))
        self.assertEqual(compute_target_size(256, 128), (1024, 512))
        self.assertEqual(compute_target_size(512, 512), (1024, 1024))
        self.assertEqual(compute_target_size(1024, 512), (2048, 1024))
        self.assertEqual(compute_target_size(1024, 1024, scale=4, max_size=2048), (2048, 2048))

    def test_seamless_wrap_around_tiling(self):
        # Create a smooth periodic 16x16 sinusoid wave that wraps cleanly in X and Y
        w = h = 16
        tile = RGBAImage.new(w, h)
        for y in range(h):
            for x in range(w):
                val = int(round(128 + 80 * math.sin(2.0 * math.pi * x / w) + 40 * math.cos(2.0 * math.pi * y / h)))
                val = max(0, min(255, val))
                tile.set_pixel(x, y, (val, val, val, 255))

        res = process_texture(tile, category="road_surface", seamless=True, scale=2)
        assert res is not None and res.normal is not None
        out = res.albedo
        ow, oh = out.width, out.height
        self.assertEqual((ow, oh), (32, 32))

        # Check horizontal and vertical wrap continuity across opposite edges
        h_diffs = [abs(out.get_pixel(0, y)[0] - out.get_pixel(ow - 1, y)[0]) for y in range(oh)]
        v_diffs = [abs(out.get_pixel(x, 0)[0] - out.get_pixel(x, oh - 1)[0]) for x in range(ow)]
        # On a 32px wave of amplitude 80, adjacent pixel step is <= 16 + micro-grain (~6)
        self.assertLess(sum(h_diffs) / len(h_diffs), 22.0)
        self.assertLess(sum(v_diffs) / len(v_diffs), 16.0)

        # Normal map should also be continuous across opposite edges
        nrm = res.normal
        nh_diffs = [abs(nrm.get_pixel(0, y)[0] - nrm.get_pixel(ow - 1, y)[0]) for y in range(oh)]
        nv_diffs = [abs(nrm.get_pixel(x, 0)[1] - nrm.get_pixel(x, oh - 1)[1]) for x in range(ow)]
        self.assertLess(sum(nh_diffs) / len(nh_diffs), 12.0)
        self.assertLess(sum(nv_diffs) / len(nv_diffs), 12.0)

    def test_alpha_halo_prevention(self):
        # Bright green foliage circle on pure BLACK transparent background (0, 0, 0, 0)
        img = RGBAImage.new(16, 16, (0, 0, 0, 0))
        for y in range(4, 12):
            for x in range(4, 12):
                img.set_pixel(x, y, (30, 210, 40, 255))

        dilated = dilate_alpha_rgb(img)
        # Adjacent transparent pixels must inherit green color, while alpha stays 0
        r, g, b, a = dilated.get_pixel(3, 6)
        self.assertEqual(a, 0)
        self.assertGreater(g, 190)
        self.assertLess(r, 45)

        # After full foliage_alpha upscale, semi-transparent edge pixels must NOT darken toward black
        res = process_texture(img, category="foliage_alpha", scale=2)
        assert res is not None
        out = res.albedo
        edge_greens = []
        for y in range(out.height):
            for x in range(out.width):
                r, g, b, a = out.get_pixel(x, y)
                if 20 <= a <= 235:
                    edge_greens.append(g)
        self.assertTrue(len(edge_greens) > 0, "Expected antialiased edge pixels")
        self.assertGreater(min(edge_greens), 175, f"Dark halo detected on alpha edge: min G={min(edge_greens)}")

    def test_normal_and_specular_maps(self):
        # Flat texture -> exact neutral tangent-space normal (128, 128, 255, 255)
        flat = RGBAImage.new(16, 16, (120, 120, 120, 255))
        nrm_flat = generate_normal_map(flat, category="buildings_props")
        self.assertEqual(nrm_flat.get_pixel(8, 8), (128, 128, 255, 255))

        # Horizontal bright-to-dark ramp -> positive nx (R > 128), neutral ny (G == 128), positive nz (B > 128)
        ramp = RGBAImage.new(16, 16)
        for y in range(16):
            for x in range(16):
                v = 240 - x * 14
                ramp.set_pixel(x, y, (v, v, v, 255))
        nrm_ramp = generate_normal_map(ramp, category="buildings_props", wrap_x=False, wrap_y=False)
        r, g, b, a = nrm_ramp.get_pixel(8, 8)
        self.assertGreater(r, 140)
        self.assertAlmostEqual(g, 128, delta=1)
        self.assertGreater(b, 128)
        self.assertEqual(a, 255)

        # Specular maps: car_livery must be much glossier/more reflective than foliage_alpha & road_surface
        paint = RGBAImage.new(16, 16, (20, 80, 210, 255))
        spec_car = generate_specular_map(paint, category="car_livery").get_pixel(8, 8)
        spec_road = generate_specular_map(paint, category="road_surface").get_pixel(8, 8)
        spec_foliage = generate_specular_map(paint, category="foliage_alpha").get_pixel(8, 8)
        self.assertEqual(spec_car[3], 255)
        self.assertEqual(spec_road[3], 255)
        self.assertEqual(spec_foliage[3], 255)
        self.assertGreater(spec_car[0], spec_road[0] + 80)  # specular strength R
        self.assertGreater(spec_car[1], spec_road[1] + 100)  # cubemap/Fresnel G
        self.assertGreater(spec_car[2], 180)  # clearcoat B
        self.assertEqual(spec_foliage[2], 0)


class TestPipelineEndToEnd(unittest.TestCase):
    def test_run_pipeline_end_to_end_and_flags(self):
        with tempfile.TemporaryDirectory() as td:
            data_dir = Path(td) / "data"
            out_dir = Path(td) / "overrides"
            (data_dir / "tracks").mkdir(parents=True)
            (data_dir / "cars").mkdir(parents=True)

            road_img = RGBAImage.new(8, 8, (90, 90, 95, 255))
            car_img = RGBAImage.new(8, 8, (20, 70, 210, 255))
            sky_img = RGBAImage.new(8, 8, (110, 170, 240, 255))
            ui_img = RGBAImage.new(8, 8, (255, 255, 255, 255))

            bfl_stage = build_synthetic_bfl(
                [
                    ("uk_tarmac01.dds", build_dds_bgra32(road_img)),
                    ("uk_sky_clouds.tga", build_tga(sky_img, bpp=24)),
                    ("hel_13pt.tga", build_tga(ui_img, bpp=32)),
                    ("ar_640a.tga", build_tga(ui_img, bpp=32)),
                ],
                compress_gzip=True,
            )
            (data_dir / "tracks" / "uk1.bfl").write_bytes(bfl_stage)
            (data_dir / "cars" / "fordbodf.tga").write_bytes(build_tga(car_img, bpp=32))

            # 1. Dry-run should not write PNGs
            dry_manifest = run_pipeline(
                data_path=data_dir,
                output_dir=out_dir,
                workers=1,
                dry_run=True,
                scale=2,
                quiet=True,
            )
            self.assertFalse(out_dir.exists())
            self.assertEqual(dry_manifest["category_counts"]["ui_skip"], 2)
            self.assertEqual(dry_manifest["category_counts"]["road_surface"], 1)
            self.assertEqual(dry_manifest["category_counts"]["car_livery"], 1)
            self.assertEqual(dry_manifest["category_counts"]["sky_backdrop"], 1)

            # 2. Full run with multiprocessing workers=2 and explicit manifest
            manifest_file = Path(td) / "custom_manifest.json"
            manifest = run_pipeline(
                data_path=data_dir,
                output_dir=out_dir,
                workers=2,
                dry_run=False,
                manifest_path=manifest_file,
                scale=2,
                quiet=True,
            )
            self.assertTrue(manifest_file.exists())
            loaded_manifest = json.loads(manifest_file.read_text(encoding="utf-8"))
            self.assertEqual(loaded_manifest["status_counts"]["processed"], 3)
            self.assertEqual(loaded_manifest["status_counts"]["skipped_ui"], 2)

            # Road & car get albedo + _n + _s
            for key in ("uk_tarmac01", "fordbodf"):
                self.assertTrue((out_dir / f"{key}.png").exists())
                self.assertTrue((out_dir / f"{key}_n.png").exists())
                self.assertTrue((out_dir / f"{key}_s.png").exists())
                self.assertEqual(load_png(out_dir / f"{key}.png").width, 16)

            # Sky gets albedo ONLY (no _n / _s)
            self.assertTrue((out_dir / "uk_sky_clouds.png").exists())
            self.assertFalse((out_dir / "uk_sky_clouds_n.png").exists())
            self.assertFalse((out_dir / "uk_sky_clouds_s.png").exists())

            # UI textures are untouched / never written
            self.assertFalse((out_dir / "hel_13pt.png").exists())
            self.assertFalse((out_dir / "ar_640a.png").exists())

            # 3. Category filter + limit check
            out_dir2 = Path(td) / "overrides_filtered"
            filtered = run_pipeline(
                data_path=data_dir,
                output_dir=out_dir2,
                categories=("car_livery",),
                limit=5,
                workers=1,
                scale=2,
                quiet=True,
            )
            self.assertEqual(filtered["status_counts"].get("processed"), 1)
            self.assertTrue((out_dir2 / "fordbodf.png").exists())
            self.assertFalse((out_dir2 / "uk_tarmac01.png").exists())


if __name__ == "__main__":
    unittest.main(verbosity=2)

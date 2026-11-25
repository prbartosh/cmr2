#!/usr/bin/env python3
"""Read-only parser and texture extractor for Colin McRae Rally 2.0 (.BFL) archives.

Exact CMR2 BFL binary format (verified against game/BFL.h, game/FileBuffer.cpp,
and game/GenericFileLoader.cpp):
  1. Outer transport: opened with zlib gzopen("rb"), so files on disk may be
     gzip-compressed (magic 1F 8B) or uncompressed.
  2. BFLHeader (8 bytes, game/BFL.h):
       char     ident[4];      // "CMPR" (0x43, 0x4D, 0x50, 0x52)
       uint32_t archiveSize;   // little-endian byte size of payload after header
     If the stream does not start with "CMPR", FileBuffer::GetGenericFileBuffer
     falls back to reading the raw file directly.
  3. Archive payload (archiveSize bytes):
       - File data blobs starting at offset 0.
       - Last 4 bytes (payload[archiveSize - 4 : archiveSize]): signed int32
         little-endian `dirOffset`, pointing to the start of the directory table.
       - Directory entries from `dirOffset` up to `archiveSize - 4`:
           uint32_t fileSize;        // named `fileSize` in BFL.h, `id` in GenericFileLoader.cpp
           int32_t  fileOffset;      // offset from payload start (after 8-byte BFLHeader)
           uint32_t fileNameLength;  // ASCII filename length (not NUL-terminated)
           char     fileName[...];   // padded to a 4-byte boundary: ((len + 3) // 4) * 4

Exact texture override naming convention (verified against game/Texture.cpp,
game/Graphics.cpp, and src/render/vector_sprite.cpp):
  - OpenCMR2 strips any directory prefix (after the last '\\' or '/'), lowercases
    the basename, and removes the `.dds` / `.tga` extension.
  - Example: `\\NEWIMAGE\\OSD\\CODRIV\\ARROWS4.TGA` -> `arrows4`
  - Example: `Cars\\Ford\\FORDBODF.DDS` -> `fordbodf`
  - Stage 6 `texture_override.cpp` looks up `overrides/<key>.png`,
    `overrides/<key>_n.png`, and `overrides/<key>_s.png`.
"""

from __future__ import annotations

import argparse
import gzip
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple, Union

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from _image_io import RGBAImage, save_png
else:
    from ._image_io import RGBAImage, save_png

BFL_MAGIC = b"CMPR"
GZIP_MAGIC = b"\x1f\x8b"
DDS_MAGIC = b"DDS "
TEXTURE_EXTENSIONS = (".dds", ".tga")


@dataclass(frozen=True)
class BFLEntry:
    """Metadata for a single file stored inside a CMR2 .BFL archive."""

    name: str
    key: str
    ext: str
    offset: int
    size: int
    raw_field0: int
    archive_path: str

    @property
    def is_texture(self) -> bool:
        return self.ext in TEXTURE_EXTENSIONS


@dataclass(frozen=True)
class TextureSource:
    """Unified handle for a texture found either inside a .BFL archive or loose on disk."""

    key: str
    original_name: str
    ext: str
    source_path: str  # path to .bfl archive or loose .dds/.tga file
    archive_member: Optional[str] = None  # entry name if inside .bfl, else None
    size: int = 0

    @property
    def relative_hint(self) -> str:
        """Combined path string useful for category classification."""
        if self.archive_member:
            return f"{self.source_path}::{self.archive_member}"
        return self.source_path


def normalize_texture_key(name: str) -> str:
    """Normalize a CMR2 texture path or archive entry name to its canonical key.

    Matches `VectorSprite_Replace` (src/render/vector_sprite.cpp:329-336),
    `Texture_GetPathStem` (game/Texture.cpp:15-20), and `CGenericFileLoader::FindFile`.
    Returns lowercase basename without `.dds` or `.tga` (or final) extension.
    """
    # Handle both Windows backslashes and POSIX forward slashes.
    base = name.replace("\\", "/").rsplit("/", 1)[-1].strip().lower()
    dot = base.rfind(".")
    if dot > 0:
        return base[:dot]
    return base


def _read_bfl_payload(raw_file: bytes) -> Tuple[bytes, bool, bool]:
    """Decompress (if gzip) and strip the 8-byte CMPR BFLHeader if present.

    Returns (payload_bytes, was_gzip, had_cmpr_header).
    """
    was_gzip = len(raw_file) >= 2 and raw_file[:2] == GZIP_MAGIC
    stream = gzip.decompress(raw_file) if was_gzip else raw_file

    if len(stream) >= 8 and stream[:4] == BFL_MAGIC:
        (archive_size,) = struct.unpack_from("<I", stream, 4)
        if archive_size == 0xFFFFFFFF:
            raise ValueError("Invalid BFL header: archiveSize == 0xFFFFFFFF")
        payload = stream[8 : 8 + archive_size]
        if len(payload) != archive_size:
            raise ValueError(
                f"Truncated BFL payload: header declares {archive_size} bytes, got {len(payload)}"
            )
        return payload, was_gzip, True

    return stream, was_gzip, False


def parse_bfl_bytes(raw_file: bytes, archive_path: str = "<memory>") -> Tuple[List[BFLEntry], bytes]:
    """Parse a CMR2 .BFL archive from bytes (read-only) and return (entries, payload)."""
    payload, _, _ = _read_bfl_payload(raw_file)
    total_len = len(payload)
    if total_len < 4:
        raise ValueError(f"BFL archive too small ({total_len} bytes): {archive_path}")

    (dir_offset,) = struct.unpack_from("<i", payload, total_len - 4)
    dir_end = total_len - 4
    if dir_offset < 0 or dir_offset > dir_end:
        raise ValueError(
            f"Invalid BFL directory offset {dir_offset} for payload size {total_len}: {archive_path}"
        )

    raw_records: List[Tuple[str, int, int]] = []
    pos = dir_offset
    while pos < dir_end:
        if pos + 12 > dir_end:
            raise ValueError(f"Truncated BFL directory entry at offset {pos}: {archive_path}")
        field0, file_offset, name_len = struct.unpack_from("<IiI", payload, pos)
        pos += 12
        if name_len > 1024 or pos + name_len > dir_end:
            raise ValueError(f"Invalid BFL entry name length {name_len} at {pos}: {archive_path}")
        name_bytes = payload[pos : pos + name_len]
        # Advance with 4-byte alignment padding (game/GenericFileLoader.cpp:72-76)
        padded_len = (name_len + 3) & ~3
        pos += padded_len
        name = name_bytes.split(b"\x00", 1)[0].decode("latin-1")
        raw_records.append((name, file_offset, field0))

    # Compute reliable byte sizes: BFL.h stores `fileSize` in field0, while some
    # non-standard tools treat field0 as an ID. Compute fallback size from the
    # distance to the next file offset (or directory start).
    sorted_offsets = sorted({off for _, off, _ in raw_records if 0 <= off <= dir_offset})
    next_offset_map: Dict[int, int] = {}
    for idx, off in enumerate(sorted_offsets):
        next_offset_map[off] = sorted_offsets[idx + 1] if idx + 1 < len(sorted_offsets) else dir_offset

    entries: List[BFLEntry] = []
    for name, file_offset, field0 in raw_records:
        if file_offset < 0 or file_offset > dir_offset:
            continue
        span_size = max(0, next_offset_map.get(file_offset, dir_offset) - file_offset)
        if 0 < field0 <= (dir_offset - file_offset):
            size = field0
        else:
            size = span_size
        lower_name = name.replace("\\", "/").rsplit("/", 1)[-1].lower()
        dot = lower_name.rfind(".")
        ext = lower_name[dot:] if dot >= 0 else ""
        key = normalize_texture_key(name)
        entries.append(
            BFLEntry(
                name=name,
                key=key,
                ext=ext,
                offset=file_offset,
                size=size,
                raw_field0=field0,
                archive_path=str(archive_path),
            )
        )
    return entries, payload


class BFLArchive:
    """Read-only reader for a CMR2 .BFL archive file."""

    def __init__(self, path: Union[str, Path]):
        self.path = Path(path)
        raw = self.path.read_bytes()
        self.entries, self._payload = parse_bfl_bytes(raw, archive_path=str(self.path))
        self._by_lower_name: Dict[str, BFLEntry] = {e.name.lower(): e for e in self.entries}

    def list_entries(self, textures_only: bool = False) -> List[BFLEntry]:
        if textures_only:
            return [e for e in self.entries if e.is_texture]
        return list(self.entries)

    def read_entry(self, entry: Union[BFLEntry, str]) -> bytes:
        if isinstance(entry, str):
            found = self._by_lower_name.get(entry.lower())
            if found is None:
                raise KeyError(f"Entry '{entry}' not found in {self.path}")
            entry = found
        return self._payload[entry.offset : entry.offset + entry.size]


# ---------------------------------------------------------------------------
# DDS Decoder (BC1/DXT1, BC2/DXT3, BC3/DXT5, and uncompressed RGB/RGBA/BGRA)
# ---------------------------------------------------------------------------


def _rgb565_to_rgb888(val: int) -> Tuple[int, int, int]:
    r5 = (val >> 11) & 0x1F
    g6 = (val >> 5) & 0x3F
    b5 = val & 0x1F
    return (
        (r5 << 3) | (r5 >> 2),
        (g6 << 2) | (g6 >> 4),
        (b5 << 3) | (b5 >> 2),
    )


def _decode_bc1_colors(c0: int, c1: int, force_opaque: bool) -> Tuple[Tuple[int, int, int, int], ...]:
    r0, g0, b0 = _rgb565_to_rgb888(c0)
    r1, g1, b1 = _rgb565_to_rgb888(c1)
    if force_opaque or c0 > c1:
        return (
            (r0, g0, b0, 255),
            (r1, g1, b1, 255),
            ((2 * r0 + r1) // 3, (2 * g0 + g1) // 3, (2 * b0 + b1) // 3, 255),
            ((r0 + 2 * r1) // 3, (g0 + 2 * g1) // 3, (b0 + 2 * b1) // 3, 255),
        )
    return (
        (r0, g0, b0, 255),
        (r1, g1, b1, 255),
        ((r0 + r1) >> 1, (g0 + g1) >> 1, (b0 + b1) >> 1, 255),
        (0, 0, 0, 0),
    )


def _decode_dxt1(data: bytes, width: int, height: int) -> RGBAImage:
    out = bytearray(width * height * 4)
    bw = max(1, (width + 3) // 4)
    bh = max(1, (height + 3) // 4)
    pos = 0
    for by in range(bh):
        py_base = by * 4
        for bx in range(bw):
            if pos + 8 > len(data):
                break
            c0, c1, bits = struct.unpack_from("<HHI", data, pos)
            pos += 8
            palette = _decode_bc1_colors(c0, c1, force_opaque=False)
            px_base = bx * 4
            for ty in range(4):
                y = py_base + ty
                if y >= height:
                    break
                row_off = y * width * 4
                shift_row = ty * 8
                for tx in range(4):
                    x = px_base + tx
                    if x >= width:
                        break
                    r, g, b, a = palette[(bits >> (shift_row + tx * 2)) & 0x3]
                    di = row_off + x * 4
                    out[di] = r
                    out[di + 1] = g
                    out[di + 2] = b
                    out[di + 3] = a
    return RGBAImage(width, height, out)


def _decode_dxt3(data: bytes, width: int, height: int) -> RGBAImage:
    out = bytearray(width * height * 4)
    bw = max(1, (width + 3) // 4)
    bh = max(1, (height + 3) // 4)
    pos = 0
    for by in range(bh):
        py_base = by * 4
        for bx in range(bw):
            if pos + 16 > len(data):
                break
            alpha_bits, c0, c1, color_bits = struct.unpack_from("<QHHI", data, pos)
            pos += 16
            palette = _decode_bc1_colors(c0, c1, force_opaque=True)
            px_base = bx * 4
            for ty in range(4):
                y = py_base + ty
                if y >= height:
                    break
                row_off = y * width * 4
                for tx in range(4):
                    x = px_base + tx
                    if x >= width:
                        break
                    pix_idx = ty * 4 + tx
                    r, g, b, _ = palette[(color_bits >> (pix_idx * 2)) & 0x3]
                    a4 = (alpha_bits >> (pix_idx * 4)) & 0xF
                    di = row_off + x * 4
                    out[di] = r
                    out[di + 1] = g
                    out[di + 2] = b
                    out[di + 3] = (a4 << 4) | a4
    return RGBAImage(width, height, out)


def _decode_dxt5(data: bytes, width: int, height: int) -> RGBAImage:
    out = bytearray(width * height * 4)
    bw = max(1, (width + 3) // 4)
    bh = max(1, (height + 3) // 4)
    pos = 0
    for by in range(bh):
        py_base = by * 4
        for bx in range(bw):
            if pos + 16 > len(data):
                break
            a0, a1 = data[pos], data[pos + 1]
            alpha_bits = int.from_bytes(data[pos + 2 : pos + 8], "little")
            c0, c1, color_bits = struct.unpack_from("<HHI", data, pos + 8)
            pos += 16
            if a0 > a1:
                alphas = (
                    a0,
                    a1,
                    (6 * a0 + 1 * a1) // 7,
                    (5 * a0 + 2 * a1) // 7,
                    (4 * a0 + 3 * a1) // 7,
                    (3 * a0 + 4 * a1) // 7,
                    (2 * a0 + 5 * a1) // 7,
                    (1 * a0 + 6 * a1) // 7,
                )
            else:
                alphas = (
                    a0,
                    a1,
                    (4 * a0 + 1 * a1) // 5,
                    (3 * a0 + 2 * a1) // 5,
                    (2 * a0 + 3 * a1) // 5,
                    (1 * a0 + 4 * a1) // 5,
                    0,
                    255,
                )
            palette = _decode_bc1_colors(c0, c1, force_opaque=True)
            px_base = bx * 4
            for ty in range(4):
                y = py_base + ty
                if y >= height:
                    break
                row_off = y * width * 4
                for tx in range(4):
                    x = px_base + tx
                    if x >= width:
                        break
                    pix_idx = ty * 4 + tx
                    r, g, b, _ = palette[(color_bits >> (pix_idx * 2)) & 0x3]
                    a = alphas[(alpha_bits >> (pix_idx * 3)) & 0x7]
                    di = row_off + x * 4
                    out[di] = r
                    out[di + 1] = g
                    out[di + 2] = b
                    out[di + 3] = a
    return RGBAImage(width, height, out)


def _mask_shift_and_max(mask: int) -> Tuple[int, int]:
    if mask == 0:
        return 0, 0
    shift = 0
    m = mask
    while (m & 1) == 0:
        m >>= 1
        shift += 1
    return shift, m


def _decode_dds_uncompressed(
    data: bytes,
    width: int,
    height: int,
    bit_count: int,
    r_mask: int,
    g_mask: int,
    b_mask: int,
    a_mask: int,
) -> RGBAImage:
    bytes_pp = bit_count // 8
    if bytes_pp not in (2, 3, 4):
        raise ValueError(f"Unsupported uncompressed DDS bit count: {bit_count}")

    # Default masks if omitted in header (matches Graphics.cpp BGRA8/BGRX8 default)
    if r_mask == 0 and g_mask == 0 and b_mask == 0:
        r_mask, g_mask, b_mask = 0x00FF0000, 0x0000FF00, 0x000000FF
        if bytes_pp == 4 and a_mask == 0:
            a_mask = 0xFF000000

    out = bytearray(width * height * 4)

    # Fast path for standard 32-bit BGRA / BGRX / RGBA and 24-bit BGR
    if bytes_pp == 4 and r_mask == 0x00FF0000 and g_mask == 0x0000FF00 and b_mask == 0x000000FF:
        has_a = a_mask == 0xFF000000
        si = 0
        di = 0
        total = width * height
        for _ in range(total):
            out[di] = data[si + 2]
            out[di + 1] = data[si + 1]
            out[di + 2] = data[si]
            out[di + 3] = data[si + 3] if has_a else 255
            si += 4
            di += 4
        return RGBAImage(width, height, out)

    if bytes_pp == 4 and r_mask == 0x000000FF and g_mask == 0x0000FF00 and b_mask == 0x00FF0000:
        has_a = a_mask == 0xFF000000
        if has_a:
            return RGBAImage(width, height, bytearray(data[: width * height * 4]))
        si = 0
        di = 0
        for _ in range(width * height):
            out[di] = data[si]
            out[di + 1] = data[si + 1]
            out[di + 2] = data[si + 2]
            out[di + 3] = 255
            si += 4
            di += 4
        return RGBAImage(width, height, out)

    if bytes_pp == 3 and r_mask == 0x00FF0000 and g_mask == 0x0000FF00 and b_mask == 0x000000FF:
        si = 0
        di = 0
        for _ in range(width * height):
            out[di] = data[si + 2]
            out[di + 1] = data[si + 1]
            out[di + 2] = data[si]
            out[di + 3] = 255
            si += 3
            di += 4
        return RGBAImage(width, height, out)

    # Generic bitmask path (handles 16-bit RGB565 / ARGB1555 / ARGB4444 and custom masks)
    r_sh, r_max = _mask_shift_and_max(r_mask)
    g_sh, g_max = _mask_shift_and_max(g_mask)
    b_sh, b_max = _mask_shift_and_max(b_mask)
    a_sh, a_max = _mask_shift_and_max(a_mask)
    si = 0
    di = 0
    for _ in range(width * height):
        pix = int.from_bytes(data[si : si + bytes_pp], "little")
        out[di] = (((pix & r_mask) >> r_sh) * 255 // r_max) if r_max else 0
        out[di + 1] = (((pix & g_mask) >> g_sh) * 255 // g_max) if g_max else 0
        out[di + 2] = (((pix & b_mask) >> b_sh) * 255 // b_max) if b_max else 0
        out[di + 3] = (((pix & a_mask) >> a_sh) * 255 // a_max) if a_max else 255
        si += bytes_pp
        di += 4
    return RGBAImage(width, height, out)


def decode_dds(raw: bytes) -> RGBAImage:
    """Decode a DirectDraw Surface (.DDS) buffer (DXT1/DXT3/DXT5 or uncompressed) to RGBAImage."""
    if len(raw) < 128 or raw[:4] != DDS_MAGIC:
        raise ValueError("Not a valid DDS file (missing 'DDS ' magic or truncated header)")

    dw_size, _dw_flags, height, width = struct.unpack_from("<IIII", raw, 4)
    if dw_size != 124:
        raise ValueError(f"Unexpected DDS header size: {dw_size} (expected 124)")
    pf_size, pf_flags, fourcc_bytes, bit_count, r_mask, g_mask, b_mask, a_mask = struct.unpack_from(
        "<II4sIIIII", raw, 76
    )
    if pf_size != 32:
        raise ValueError(f"Unexpected DDS pixel format size: {pf_size}")

    pixel_data = raw[128:]
    is_fourcc = (pf_flags & 0x4) != 0 or fourcc_bytes in (b"DXT1", b"DXT2", b"DXT3", b"DXT4", b"DXT5")
    if is_fourcc:
        if fourcc_bytes == b"DXT1":
            return _decode_dxt1(pixel_data, width, height)
        if fourcc_bytes in (b"DXT2", b"DXT3"):
            return _decode_dxt3(pixel_data, width, height)
        if fourcc_bytes in (b"DXT4", b"DXT5"):
            return _decode_dxt5(pixel_data, width, height)
        raise ValueError(f"Unsupported DDS FourCC: {fourcc_bytes!r}")

    return _decode_dds_uncompressed(pixel_data, width, height, bit_count, r_mask, g_mask, b_mask, a_mask)


# ---------------------------------------------------------------------------
# TGA Decoder (24/32-bit uncompressed + RLE + grayscale)
# ---------------------------------------------------------------------------


def decode_tga(raw: bytes) -> RGBAImage:
    """Decode a Truevision TGA buffer (24/32-bit uncompressed or RLE, bottom-up or top-down)."""
    if len(raw) < 18:
        raise ValueError("TGA file too small (< 18 bytes)")

    (
        id_len,
        cmap_type,
        img_type,
        _cmap_first,
        cmap_len,
        cmap_depth,
        _x_orig,
        _y_orig,
        width,
        height,
        bpp,
        descriptor,
    ) = struct.unpack_from("<BBBHHBHHHHBB", raw, 0)

    if cmap_type != 0:
        cmap_bytes = cmap_len * ((cmap_depth + 7) // 8)
    else:
        cmap_bytes = 0

    if width == 0 or height == 0:
        raise ValueError(f"Invalid TGA dimensions: {width}x{height}")

    bytes_pp = bpp // 8
    if bytes_pp not in (1, 3, 4):
        raise ValueError(f"Unsupported TGA bits-per-pixel: {bpp}")

    src = 18 + id_len + cmap_bytes
    total_pixels = width * height
    raw_pixels = bytearray(total_pixels * bytes_pp)

    if img_type in (2, 3):  # Uncompressed true-color or grayscale
        need = total_pixels * bytes_pp
        if src + need > len(raw):
            raise ValueError(f"Truncated TGA pixel data: need {need}, have {len(raw) - src}")
        raw_pixels[:] = raw[src : src + need]
    elif img_type in (10, 11):  # RLE true-color or grayscale
        dst_pix = 0
        pos = src
        while dst_pix < total_pixels and pos < len(raw):
            header = raw[pos]
            pos += 1
            count = (header & 0x7F) + 1
            if header & 0x80:
                pix = raw[pos : pos + bytes_pp]
                pos += bytes_pp
                for _ in range(count):
                    if dst_pix >= total_pixels:
                        break
                    raw_pixels[dst_pix * bytes_pp : (dst_pix + 1) * bytes_pp] = pix
                    dst_pix += 1
            else:
                chunk = raw[pos : pos + count * bytes_pp]
                pos += count * bytes_pp
                take = min(count, total_pixels - dst_pix)
                raw_pixels[dst_pix * bytes_pp : (dst_pix + take) * bytes_pp] = chunk[: take * bytes_pp]
                dst_pix += take
    else:
        raise ValueError(f"Unsupported TGA image type: {img_type}")

    top_down = (descriptor & 0x20) != 0
    right_to_left = (descriptor & 0x10) != 0
    out = bytearray(total_pixels * 4)

    for y in range(height):
        src_y = y if top_down else (height - 1 - y)
        row_src = src_y * width * bytes_pp
        row_dst = y * width * 4
        for x in range(width):
            src_x = (width - 1 - x) if right_to_left else x
            si = row_src + src_x * bytes_pp
            di = row_dst + x * 4
            if bytes_pp == 4:
                out[di] = raw_pixels[si + 2]
                out[di + 1] = raw_pixels[si + 1]
                out[di + 2] = raw_pixels[si]
                out[di + 3] = raw_pixels[si + 3]
            elif bytes_pp == 3:
                out[di] = raw_pixels[si + 2]
                out[di + 1] = raw_pixels[si + 1]
                out[di + 2] = raw_pixels[si]
                out[di + 3] = 255
            else:
                v = raw_pixels[si]
                out[di] = v
                out[di + 1] = v
                out[di + 2] = v
                out[di + 3] = 255

    return RGBAImage(width, height, out)


def decode_texture_bytes(raw: bytes, ext: str) -> RGBAImage:
    """Decode raw DDS or TGA bytes (auto-detecting DDS magic if present)."""
    ext_l = ext.lower()
    if len(raw) >= 4 and raw[:4] == DDS_MAGIC:
        return decode_dds(raw)
    if ext_l == ".dds":
        return decode_dds(raw)
    if ext_l == ".tga":
        return decode_tga(raw)
    raise ValueError(f"Unsupported texture format: '{ext}'")


def load_texture_source(src: TextureSource) -> RGBAImage:
    """Read and decode a TextureSource (from a .BFL archive or loose file) into an RGBAImage."""
    if src.archive_member is not None:
        archive = BFLArchive(src.source_path)
        raw = archive.read_entry(src.archive_member)
        return decode_texture_bytes(raw, src.ext)
    raw = Path(src.source_path).read_bytes()
    return decode_texture_bytes(raw, src.ext)


def scan_data_directory(
    root: Union[str, Path],
    include_loose: bool = True,
) -> Tuple[List[TextureSource], List[str]]:
    """Scan a game data directory (or single .BFL file) for all DDS/TGA textures.

    Returns (sources, warnings). Never modifies any source files.
    """
    root_path = Path(root)
    sources: List[TextureSource] = []
    warnings: List[str] = []

    if root_path.is_file():
        files = [root_path]
    elif root_path.is_dir():
        files = sorted(p for p in root_path.rglob("*") if p.is_file())
    else:
        raise FileNotFoundError(f"Input path does not exist: {root_path}")

    for path in files:
        suffix = path.suffix.lower()
        if suffix == ".bfl":
            try:
                archive = BFLArchive(path)
                for entry in archive.list_entries(textures_only=True):
                    sources.append(
                        TextureSource(
                            key=entry.key,
                            original_name=entry.name,
                            ext=entry.ext,
                            source_path=str(path),
                            archive_member=entry.name,
                            size=entry.size,
                        )
                    )
            except Exception as exc:
                warnings.append(f"Skipping unreadable BFL '{path}': {exc}")
        elif include_loose and suffix in TEXTURE_EXTENSIONS:
            sources.append(
                TextureSource(
                    key=normalize_texture_key(path.name),
                    original_name=path.name,
                    ext=suffix,
                    source_path=str(path),
                    archive_member=None,
                    size=path.stat().st_size,
                )
            )

    return sources, warnings


def extract_textures(
    input_path: Union[str, Path],
    output_dir: Optional[Union[str, Path]] = None,
    decode_png: bool = True,
    keep_raw: bool = False,
    limit: Optional[int] = None,
) -> List[Dict[str, object]]:
    """Catalog and optionally extract textures from a .BFL file or directory."""
    sources, warnings = scan_data_directory(input_path)
    for w in warnings:
        print(f"[warn] {w}", file=sys.stderr)

    catalog: List[Dict[str, object]] = []
    out_root = Path(output_dir) if output_dir else None
    seen_keys: Dict[str, int] = {}

    # Cache parsed BFL archives while iterating sequentially
    archive_cache: Dict[str, BFLArchive] = {}

    for src in sources:
        if limit is not None and len(catalog) >= limit:
            break
        record: Dict[str, object] = {
            "key": src.key,
            "original_name": src.original_name,
            "ext": src.ext,
            "source_path": src.source_path,
            "archive_member": src.archive_member,
            "size": src.size,
        }
        seen_keys[src.key] = seen_keys.get(src.key, 0) + 1
        record["duplicate_index"] = seen_keys[src.key] - 1

        if out_root is not None:
            if src.archive_member is not None:
                arch = archive_cache.get(src.source_path)
                if arch is None:
                    arch = BFLArchive(src.source_path)
                    archive_cache[src.source_path] = arch
                raw = arch.read_entry(src.archive_member)
            else:
                raw = Path(src.source_path).read_bytes()

            if keep_raw:
                raw_path = out_root / f"{src.key}{src.ext}"
                raw_path.parent.mkdir(parents=True, exist_ok=True)
                raw_path.write_bytes(raw)
                record["raw_output"] = str(raw_path)

            if decode_png:
                try:
                    img = decode_texture_bytes(raw, src.ext)
                    png_path = save_png(img, out_root / f"{src.key}.png")
                    record["width"] = img.width
                    record["height"] = img.height
                    record["png_output"] = str(png_path)
                except Exception as exc:
                    record["decode_error"] = str(exc)

        catalog.append(record)
    return catalog


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Read-only CMR2 .BFL archive parser and DDS/TGA texture extractor."
    )
    parser.add_argument("input", help="Path to a .BFL file or CMR2 game data directory")
    parser.add_argument("-o", "--output", help="Output directory to write extracted textures")
    parser.add_argument("--list", action="store_true", help="List textures without writing files")
    parser.add_argument(
        "--raw", action="store_true", help="Also extract raw .dds/.tga files alongside decoded PNGs"
    )
    parser.add_argument("--no-png", action="store_true", help="Skip decoding DDS/TGA to RGBA PNG")
    parser.add_argument("--limit", type=int, default=None, help="Maximum number of textures to process")
    parser.add_argument("--json", dest="json_out", help="Optional path to write JSON catalog")
    args = parser.parse_args(list(argv) if argv is not None else None)

    out_dir = None if args.list else args.output
    catalog = extract_textures(
        args.input,
        output_dir=out_dir,
        decode_png=not args.no_png,
        keep_raw=args.raw,
        limit=args.limit,
    )

    if args.json_out:
        Path(args.json_out).write_text(json.dumps(catalog, indent=2), encoding="utf-8")

    for rec in catalog:
        src_desc = (
            f"{Path(str(rec['source_path'])).name}::{rec['archive_member']}"
            if rec["archive_member"]
            else str(rec["source_path"])
        )
        dims = f" {rec['width']}x{rec['height']}" if "width" in rec else ""
        err = f" ERROR={rec['decode_error']}" if "decode_error" in rec else ""
        print(f"{rec['key']}{rec['ext']} ({rec['size']} B{dims}) <- {src_desc}{err}")

    print(f"Total textures: {len(catalog)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

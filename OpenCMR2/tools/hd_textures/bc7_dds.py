#!/usr/bin/env python3
"""DX10 DDS / BC7 encoder, validator, and full mipmap chain generator for OpenCMR2.

Produces native GPU-compressed BC7 (`DXGI_FORMAT_BC7_UNORM = 98`) DX10 DDS files
with complete pre-generated mipmap chains down to 1x1:
  - Albedo / alpha textures : alpha-weighted 2x2 box downsample (matches
                              `texture_override.cpp` so foliage/fences never darken)
  - Normal maps             : tangent-space vector average + unit re-normalization
  - Specular / PBR maps     : 2x2 box downsample

BC7 encoding backend priority (auto-selected fastest first):
  1. Native multithreaded C++ `libbc7enc.dylib` / `.so` via `ctypes`
     (auto-discovered via `OPENCMR2_BC7_LIB`, `tools/hd_textures/libbc7enc.dylib`,
     or `~/.cache/opencmr2_hd/bc7enc/libbc7enc.dylib`).
  2. External CLI BC7 encoders in PATH (`bc7enc`, `texconv`, `compressonatorcli`,
     `cuttlefish`).
  3. Zero-dependency BC7 Mode 6 encoder (NumPy-accelerated when installed, or
     100% pure Python stdlib fallback).
"""

from __future__ import annotations

import ctypes
import math
import os
import shutil
import subprocess
import struct
import sys
import tempfile
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple, Union

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from _image_io import HAS_NUMPY, RGBAImage, save_png
else:
    from ._image_io import HAS_NUMPY, RGBAImage, save_png

if HAS_NUMPY:
    import numpy as np  # type: ignore

DDS_MAGIC = b"DDS "
DDS_HEADER_SIZE = 124
DDS_PIXELFORMAT_SIZE = 32
DDS_DXT10_HEADER_SIZE = 20
DDS_BC7_HEADER_BYTES = 4 + DDS_HEADER_SIZE + DDS_DXT10_HEADER_SIZE  # 148 bytes

# DDS_HEADER.dwFlags
DDSD_CAPS = 0x1
DDSD_HEIGHT = 0x2
DDSD_WIDTH = 0x4
DDSD_PIXELFORMAT = 0x1000
DDSD_MIPMAPCOUNT = 0x20000
DDSD_LINEARSIZE = 0x80000
DDS_BC7_FLAGS = DDSD_CAPS | DDSD_HEIGHT | DDSD_WIDTH | DDSD_PIXELFORMAT | DDSD_MIPMAPCOUNT | DDSD_LINEARSIZE

# DDS_PIXELFORMAT.dwFlags
DDPF_FOURCC = 0x4

# DDS_HEADER.dwCaps
DDSCAPS_COMPLEX = 0x8
DDSCAPS_TEXTURE = 0x1000
DDSCAPS_MIPMAP = 0x400000
DDS_BC7_CAPS = DDSCAPS_TEXTURE | DDSCAPS_COMPLEX | DDSCAPS_MIPMAP  # 0x401008

# DDS_HEADER_DXT10
DXGI_FORMAT_BC7_UNORM = 98
DXGI_FORMAT_BC7_UNORM_SRGB = 99
D3D10_RESOURCE_DIMENSION_TEXTURE2D = 3

# BC7 4-bit interpolation weights (Direct3D 11 / Khronos Data Format Spec aWeight4)
BC7_WEIGHTS4: Tuple[int, ...] = (0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64)

BC7_QUALITY_CHOICES = ("ultra_fast", "fast", "medium", "high")

# Mapping from quality preset -> (uber_level 0..4, max_partitions 0..64)
BC7_QUALITY_PARAMS: Dict[str, Tuple[int, int]] = {
    "ultra_fast": (0, 0),
    "fast": (0, 16),
    "medium": (1, 32),
    "high": (3, 64),
}

_NATIVE_LIB_CACHE: Optional[Tuple[Optional[ctypes.CDLL], Optional[Path]]] = None


@dataclass(frozen=True)
class DDSBC7Header:
    """Parsed metadata from a DX10 BC7 DDS file."""

    width: int
    height: int
    mip_count: int
    linear_size: int
    flags: int
    caps: int
    fourcc: bytes
    dxgi_format: int
    resource_dimension: int
    misc_flag: int
    array_size: int
    misc_flags2: int
    mip_sizes: Tuple[Tuple[int, int, int], ...]  # (mip_w, mip_h, byte_len)
    data_offset: int
    total_expected_bytes: int


def compute_mip_dimensions(width: int, height: int) -> List[Tuple[int, int]]:
    """Return full mipmap chain dimensions [(w, h), ..., (1, 1)]."""
    if width <= 0 or height <= 0:
        raise ValueError(f"Invalid texture dimensions: {width}x{height}")
    dims: List[Tuple[int, int]] = []
    mw, mh = int(width), int(height)
    while True:
        dims.append((mw, mh))
        if mw == 1 and mh == 1:
            break
        mw = max(1, mw >> 1)
        mh = max(1, mh >> 1)
    return dims


def bc7_mip_byte_size(width: int, height: int) -> int:
    """Return tightly packed BC7 byte size for a single mip level."""
    return ((width + 3) // 4) * ((height + 3) // 4) * 16


def build_dds_bc7_header(
    width: int,
    height: int,
    mip_count: int,
    dxgi_format: int = DXGI_FORMAT_BC7_UNORM,
) -> bytes:
    """Build the 148-byte DX10 DDS header for BC7 textures (matches OpenCMR2 loader)."""
    if width <= 0 or height <= 0 or mip_count <= 0:
        raise ValueError(f"Invalid DDS header parameters: {width}x{height}, mips={mip_count}")

    linear_size = bc7_mip_byte_size(width, height)

    # DDS_HEADER (124 bytes)
    header_part1 = struct.pack(
        "<IIIIIII",
        DDS_HEADER_SIZE,
        DDS_BC7_FLAGS,
        height,
        width,
        linear_size,
        1,  # dwDepth
        mip_count,
    )
    reserved1 = b"\x00" * (11 * 4)
    ddspf = struct.pack(
        "<II4sIIIII",
        DDS_PIXELFORMAT_SIZE,
        DDPF_FOURCC,
        b"DX10",
        0,
        0,
        0,
        0,
        0,
    )
    caps_block = struct.pack(
        "<IIIII",
        DDS_BC7_CAPS,
        0,  # dwCaps2
        0,  # dwCaps3
        0,  # dwCaps4
        0,  # dwReserved2
    )
    # DDS_HEADER_DXT10 (20 bytes)
    dx10 = struct.pack(
        "<IIIII",
        dxgi_format,
        D3D10_RESOURCE_DIMENSION_TEXTURE2D,
        0,  # miscFlag
        1,  # arraySize
        0,  # miscFlags2
    )
    hdr = DDS_MAGIC + header_part1 + reserved1 + ddspf + caps_block + dx10
    assert len(hdr) == DDS_BC7_HEADER_BYTES
    return hdr


def parse_dds_bc7_header(raw_bytes: bytes) -> DDSBC7Header:
    """Parse and validate a DX10 BC7 DDS binary payload."""
    if len(raw_bytes) < DDS_BC7_HEADER_BYTES:
        raise ValueError(f"DDS data too short ({len(raw_bytes)} bytes, need >= {DDS_BC7_HEADER_BYTES})")
    if raw_bytes[:4] != DDS_MAGIC:
        raise ValueError(f"Invalid DDS magic: {raw_bytes[:4]!r}")

    dw_size, dw_flags, height, width, linear_size, _depth, mip_count = struct.unpack_from("<IIIIIII", raw_bytes, 4)
    if dw_size != DDS_HEADER_SIZE:
        raise ValueError(f"Unexpected DDS_HEADER.dwSize={dw_size} (expected {DDS_HEADER_SIZE})")
    if width == 0 or height == 0:
        raise ValueError(f"Invalid DDS dimensions: {width}x{height}")

    pf_size, pf_flags, fourcc = struct.unpack_from("<II4s", raw_bytes, 76)
    if pf_size != DDS_PIXELFORMAT_SIZE:
        raise ValueError(f"Unexpected DDS_PIXELFORMAT.dwSize={pf_size} (expected {DDS_PIXELFORMAT_SIZE})")
    if (pf_flags & DDPF_FOURCC) == 0 or fourcc != b"DX10":
        raise ValueError(f"Expected DX10 FOURCC header, got flags=0x{pf_flags:x}, fourcc={fourcc!r}")

    (dw_caps,) = struct.unpack_from("<I", raw_bytes, 108)
    dxgi_format, res_dim, misc_flag, array_size, misc_flags2 = struct.unpack_from("<IIIII", raw_bytes, 128)
    if dxgi_format not in (DXGI_FORMAT_BC7_UNORM, DXGI_FORMAT_BC7_UNORM_SRGB):
        raise ValueError(f"Expected DXGI_FORMAT_BC7_UNORM(98/99), got {dxgi_format}")
    if res_dim != D3D10_RESOURCE_DIMENSION_TEXTURE2D:
        raise ValueError(f"Expected TEXTURE2D(3), got resourceDimension={res_dim}")

    eff_mips = max(1, mip_count)
    mip_sizes: List[Tuple[int, int, int]] = []
    mw, mh = width, height
    data_bytes = 0
    for _ in range(eff_mips):
        sz = bc7_mip_byte_size(mw, mh)
        mip_sizes.append((mw, mh, sz))
        data_bytes += sz
        mw = max(1, mw >> 1)
        mh = max(1, mh >> 1)

    total_expected = DDS_BC7_HEADER_BYTES + data_bytes * max(1, array_size)
    if len(raw_bytes) < total_expected:
        raise ValueError(
            f"Truncated BC7 DDS payload: expected at least {total_expected} bytes, got {len(raw_bytes)}"
        )

    return DDSBC7Header(
        width=width,
        height=height,
        mip_count=eff_mips,
        linear_size=linear_size,
        flags=dw_flags,
        caps=dw_caps,
        fourcc=fourcc,
        dxgi_format=dxgi_format,
        resource_dimension=res_dim,
        misc_flag=misc_flag,
        array_size=array_size,
        misc_flags2=misc_flags2,
        mip_sizes=tuple(mip_sizes),
        data_offset=DDS_BC7_HEADER_BYTES,
        total_expected_bytes=total_expected,
    )


# ---------------------------------------------------------------------------
# Pre-generated mipmap chain downsampling
# ---------------------------------------------------------------------------


def downsample_mip_2x(
    image: RGBAImage,
    is_normal_map: bool = False,
    is_specular_map: bool = False,
) -> RGBAImage:
    """Downsample an RGBAImage by 2x for the next mipmap level.

    Matches `DownsampleBGRAAlphaWeighted` in OpenCMR2 `src/render/texture_override.cpp`:
      - `is_normal_map=True`: tangent-space vector average + unit re-normalization.
      - `is_specular_map=True`: standard 2x2 box average (A=255).
      - Default (albedo/alpha): alpha-weighted 2x2 box average so foliage/fences
        never develop dark borders at lower mip levels.
    """
    sw, sh = image.width, image.height
    dw = max(1, sw // 2)
    dh = max(1, sh // 2)

    if HAS_NUMPY:
        arr = np.frombuffer(image.data, dtype=np.uint8).reshape((sh, sw, 4))
        # Build 2x2 tap coordinates matching std::min(s - 1, dst * 2 + d)
        y0 = np.arange(dh) * 2
        y1 = np.minimum(sh - 1, y0 + 1)
        x0 = np.arange(dw) * 2
        x1 = np.minimum(sw - 1, x0 + 1)

        p00 = arr[np.ix_(y0, x0)].astype(np.float32)
        p01 = arr[np.ix_(y0, x1)].astype(np.float32)
        p10 = arr[np.ix_(y1, x0)].astype(np.float32)
        p11 = arr[np.ix_(y1, x1)].astype(np.float32)

        if is_normal_map:
            s = (p00[:, :, :3] + p01[:, :, :3] + p10[:, :, :3] + p11[:, :, :3]) * (2.0 / 255.0) - 4.0
            nx, ny, nz = s[:, :, 0], s[:, :, 1], s[:, :, 2]
            length = np.sqrt(nx * nx + ny * ny + nz * nz)
            valid = length > 1e-5
            safe_len = np.where(valid, length, 1.0)
            nx = np.where(valid, nx / safe_len, 0.0)
            ny = np.where(valid, ny / safe_len, 0.0)
            nz = np.where(valid, nz / safe_len, 1.0)
            out = np.empty((dh, dw, 4), dtype=np.uint8)
            out[:, :, 0] = np.clip(np.rint((nx * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
            out[:, :, 1] = np.clip(np.rint((ny * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
            out[:, :, 2] = np.clip(np.rint((nz * 0.5 + 0.5) * 255.0), 0, 255).astype(np.uint8)
            out[:, :, 3] = 255
            return RGBAImage(dw, dh, bytearray(out.tobytes()))

        if is_specular_map:
            avg = (p00 + p01 + p10 + p11) * 0.25
            out = np.empty((dh, dw, 4), dtype=np.uint8)
            out[:, :, :3] = np.clip(np.rint(avg[:, :, :3]), 0, 255).astype(np.uint8)
            out[:, :, 3] = 255
            return RGBAImage(dw, dh, bytearray(out.tobytes()))

        # Alpha-weighted box downsample (exact integer-equivalent rounding)
        a00 = p00[:, :, 3:4]
        a01 = p01[:, :, 3:4]
        a10 = p10[:, :, 3:4]
        a11 = p11[:, :, 3:4]
        a_sum = a00 + a01 + a10 + a11
        w_rgb = p00[:, :, :3] * a00 + p01[:, :, :3] * a01 + p10[:, :, :3] * a10 + p11[:, :, :3] * a11
        avg_rgb = (p00[:, :, :3] + p01[:, :, :3] + p10[:, :, :3] + p11[:, :, :3]) * 0.25
        rgb = np.where(a_sum > 0.0, w_rgb / np.maximum(a_sum, 1.0), avg_rgb)
        out = np.empty((dh, dw, 4), dtype=np.uint8)
        out[:, :, :3] = np.clip(np.rint(rgb), 0, 255).astype(np.uint8)
        out[:, :, 3] = np.clip(np.rint(a_sum[:, :, 0] * 0.25), 0, 255).astype(np.uint8)
        return RGBAImage(dw, dh, bytearray(out.tobytes()))

    # Pure-Python fallback
    src = image.data
    out = bytearray(dw * dh * 4)
    inv255_2 = 2.0 / 255.0

    for y in range(dh):
        sy0 = y * 2
        sy1 = sy0 + 1 if sy0 + 1 < sh else sh - 1
        row0 = sy0 * sw * 4
        row1 = sy1 * sw * 4
        drow = y * dw * 4
        for x in range(dw):
            sx0 = x * 2
            sx1 = sx0 + 1 if sx0 + 1 < sw else sw - 1
            i00 = row0 + sx0 * 4
            i01 = row0 + sx1 * 4
            i10 = row1 + sx0 * 4
            i11 = row1 + sx1 * 4
            di = drow + x * 4

            if is_normal_map:
                nx = (src[i00] + src[i01] + src[i10] + src[i11]) * inv255_2 - 4.0
                ny = (src[i00 + 1] + src[i01 + 1] + src[i10 + 1] + src[i11 + 1]) * inv255_2 - 4.0
                nz = (src[i00 + 2] + src[i01 + 2] + src[i10 + 2] + src[i11 + 2]) * inv255_2 - 4.0
                length = math.sqrt(nx * nx + ny * ny + nz * nz)
                if length > 1e-5:
                    inv = 1.0 / length
                    nx *= inv
                    ny *= inv
                    nz *= inv
                else:
                    nx, ny, nz = 0.0, 0.0, 1.0
                out[di] = min(255, max(0, int(round((nx * 0.5 + 0.5) * 255.0))))
                out[di + 1] = min(255, max(0, int(round((ny * 0.5 + 0.5) * 255.0))))
                out[di + 2] = min(255, max(0, int(round((nz * 0.5 + 0.5) * 255.0))))
                out[di + 3] = 255
            elif is_specular_map:
                out[di] = (src[i00] + src[i01] + src[i10] + src[i11] + 2) >> 2
                out[di + 1] = (src[i00 + 1] + src[i01 + 1] + src[i10 + 1] + src[i11 + 1] + 2) >> 2
                out[di + 2] = (src[i00 + 2] + src[i01 + 2] + src[i10 + 2] + src[i11 + 2] + 2) >> 2
                out[di + 3] = 255
            else:
                a00, a01, a10, a11 = src[i00 + 3], src[i01 + 3], src[i10 + 3], src[i11 + 3]
                a_sum = a00 + a01 + a10 + a11
                if a_sum > 0:
                    half = a_sum >> 1
                    out[di] = (src[i00] * a00 + src[i01] * a01 + src[i10] * a10 + src[i11] * a11 + half) // a_sum
                    out[di + 1] = (
                        src[i00 + 1] * a00 + src[i01 + 1] * a01 + src[i10 + 1] * a10 + src[i11 + 1] * a11 + half
                    ) // a_sum
                    out[di + 2] = (
                        src[i00 + 2] * a00 + src[i01 + 2] * a01 + src[i10 + 2] * a10 + src[i11 + 2] * a11 + half
                    ) // a_sum
                    out[di + 3] = (a_sum + 2) >> 2
                else:
                    out[di] = (src[i00] + src[i01] + src[i10] + src[i11] + 2) >> 2
                    out[di + 1] = (src[i00 + 1] + src[i01 + 1] + src[i10 + 1] + src[i11 + 1] + 2) >> 2
                    out[di + 2] = (src[i00 + 2] + src[i01 + 2] + src[i10 + 2] + src[i11 + 2] + 2) >> 2
                    out[di + 3] = 0

    return RGBAImage(dw, dh, out)


def generate_mipmaps(
    image: RGBAImage,
    is_normal_map: bool = False,
    is_specular_map: bool = False,
) -> List[RGBAImage]:
    """Generate a full mipmap chain from level 0 down to 1x1."""
    mips: List[RGBAImage] = [image]
    cur = image
    while cur.width > 1 or cur.height > 1:
        cur = downsample_mip_2x(cur, is_normal_map=is_normal_map, is_specular_map=is_specular_map)
        mips.append(cur)
    return mips


# ---------------------------------------------------------------------------
# Backend 1: Native multithreaded C++ libbc7enc.dylib / .so via ctypes
# ---------------------------------------------------------------------------


def find_bc7_library() -> Optional[Path]:
    """Locate native `libbc7enc.dylib` / `.so` / `.dll` if installed."""
    candidates: List[Path] = []
    env_lib = os.environ.get("OPENCMR2_BC7_LIB")
    if env_lib:
        candidates.append(Path(env_lib).expanduser())

    here = Path(__file__).resolve().parent
    cache_dir = Path.home() / ".cache" / "opencmr2_hd" / "bc7enc"
    for lib_name in ("libbc7enc.dylib", "libbc7enc.so", "bc7enc.dll"):
        candidates.append(here / lib_name)
        candidates.append(cache_dir / lib_name)

    for cand in candidates:
        if cand.is_file():
            return cand
    return None


def _load_native_bc7() -> Tuple[Optional[ctypes.CDLL], Optional[Path]]:
    global _NATIVE_LIB_CACHE
    if _NATIVE_LIB_CACHE is not None:
        return _NATIVE_LIB_CACHE

    lib_path = find_bc7_library()
    if lib_path is None:
        _NATIVE_LIB_CACHE = (None, None)
        return _NATIVE_LIB_CACHE

    try:
        lib = ctypes.CDLL(str(lib_path))
        fn = lib.bc7_encode_rgba_mt
        # Exact 8-argument C ABI:
        # void bc7_encode_rgba_mt(const uint8_t *rgba, int width, int height, uint8_t *out_blocks,
        #                         int uber_level, int max_partitions, int perceptual, int num_threads);
        fn.argtypes = [
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_void_p,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
            ctypes.c_int,
        ]
        fn.restype = None
        _NATIVE_LIB_CACHE = (lib, lib_path)
    except Exception:
        _NATIVE_LIB_CACHE = (None, None)
    return _NATIVE_LIB_CACHE


def encode_bc7_native(
    image: RGBAImage,
    quality: str = "fast",
    perceptual: bool = True,
    num_threads: int = 0,
) -> bytes:
    """Encode an RGBAImage into tightly packed BC7 blocks via native libbc7enc."""
    lib, _ = _load_native_bc7()
    if lib is None:
        raise RuntimeError("Native libbc7enc library not available")

    uber_level, max_partitions = BC7_QUALITY_PARAMS.get(quality, BC7_QUALITY_PARAMS["fast"])
    out_len = bc7_mip_byte_size(image.width, image.height)

    src_buf = (ctypes.c_uint8 * len(image.data)).from_buffer(image.data)
    out_buf = (ctypes.c_uint8 * out_len)()

    lib.bc7_encode_rgba_mt(
        ctypes.byref(src_buf),
        int(image.width),
        int(image.height),
        ctypes.byref(out_buf),
        int(uber_level),
        int(max_partitions),
        1 if perceptual else 0,
        int(num_threads),
    )
    return bytes(out_buf)


# ---------------------------------------------------------------------------
# Backend 2: External CLI encoders (bc7enc / texconv / compressonatorcli / cuttlefish)
# ---------------------------------------------------------------------------


def find_bc7_cli() -> Optional[Tuple[str, Path]]:
    """Auto-detect an external CLI BC7 encoder in PATH if installed."""
    for name in ("bc7enc", "texconv", "compressonatorcli", "cuttlefish"):
        found = shutil.which(name)
        if found:
            return (name, Path(found))
    return None


def _extract_dds_mip0_blocks(dds_bytes: bytes, expected_len: int) -> Optional[bytes]:
    if len(dds_bytes) < 128 or dds_bytes[:4] != DDS_MAGIC:
        return None
    fourcc = dds_bytes[84:88]
    offset = 148 if fourcc == b"DX10" else 128
    if len(dds_bytes) < offset + expected_len:
        return None
    return dds_bytes[offset : offset + expected_len]


def encode_bc7_cli(image: RGBAImage, quality: str = "fast") -> bytes:
    """Encode a single mip level using an external CLI BC7 encoder."""
    found = find_bc7_cli()
    if found is None:
        raise RuntimeError("No external BC7 CLI encoder found in PATH")
    tool_name, tool_path = found
    expected_len = bc7_mip_byte_size(image.width, image.height)

    with tempfile.TemporaryDirectory(prefix="opencmr2_bc7_") as td:
        td_path = Path(td)
        in_png = td_path / "in.png"
        out_dds = td_path / "in.dds"
        save_png(image, in_png, compress_level=1)

        if tool_name == "bc7enc":
            uber, _ = BC7_QUALITY_PARAMS.get(quality, BC7_QUALITY_PARAMS["fast"])
            cmd = [str(tool_path), "-g", f"-u{uber}", str(in_png), str(out_dds)]
        elif tool_name == "texconv":
            cmd = [str(tool_path), "-f", "BC7_UNORM", "-m", "1", "-y", "-o", str(td_path), str(in_png)]
        elif tool_name == "compressonatorcli":
            cmd = [str(tool_path), "-fd", "BC7", "-miplevels", "1", str(in_png), str(out_dds)]
        else:  # cuttlefish
            cmd = [str(tool_path), "-i", str(in_png), "-o", str(out_dds), "-f", "BC7", "-t", "u8"]

        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if proc.returncode != 0 or not out_dds.is_file():
            raise RuntimeError(f"{tool_name} failed (rc={proc.returncode}): {proc.stderr.decode('utf-8', 'ignore')}")

        blocks = _extract_dds_mip0_blocks(out_dds.read_bytes(), expected_len)
        if blocks is None:
            raise RuntimeError(f"{tool_name} produced unexpected DDS output")
        return blocks


# ---------------------------------------------------------------------------
# Backend 3: Zero-dependency BC7 Mode 6 encoder & validator
# ---------------------------------------------------------------------------


def _quantize_mode6_endpoint(r: float, g: float, b: float, a: float) -> Tuple[int, int, int, int, int]:
    """Quantize an RGBA8 endpoint to Mode 6 (7-bit R, G, B, A + shared 1-bit p-bit).

    In BC7 Mode 6 each endpoint stores 7 bits per channel plus 1 unique p-bit
    shared across its 4 channels; decoded 8-bit endpoint value is `(c7 << 1) | p`.
    Returns `(r7, g7, b7, a7, pbit)` minimizing squared RGBA quantization error.
    """
    best_err = 1e18
    best = (0, 0, 0, 0, 0)
    for p in (0, 1):
        r7 = min(127, max(0, int(round((r - p) * 0.5))))
        g7 = min(127, max(0, int(round((g - p) * 0.5))))
        b7 = min(127, max(0, int(round((b - p) * 0.5))))
        a7 = min(127, max(0, int(round((a - p) * 0.5))))
        dr = ((r7 << 1) | p) - r
        dg = ((g7 << 1) | p) - g
        db = ((b7 << 1) | p) - b
        da = ((a7 << 1) | p) - a
        err = dr * dr + dg * dg + db * db + da * da
        if err < best_err:
            best_err = err
            best = (r7, g7, b7, a7, p)
    return best


def encode_bc7_mode6_block(pixels_16: Sequence[Tuple[int, int, int, int]]) -> bytes:
    """Encode 16 RGBA8 pixels (4x4 row-major) into one bit-exact 16-byte BC7 Mode 6 block.

    Bit layout (128 bits, little-endian):
      - bits   0..  6 ( 7 bits): mode 6 indicator `0b1000000` (value 64)
      - bits   7.. 62 (56 bits): 7-bit endpoints R0, R1, G0, G1, B0, B1, A0, A1
      - bits  63.. 64 ( 2 bits): unique p-bits P0, P1
      - bits  65.. 67 ( 3 bits): anchor index 0 (MSB guaranteed == 0)
      - bits  68..127 (60 bits): 4-bit indices 1..15
    """
    if len(pixels_16) != 16:
        raise ValueError(f"Expected 16 pixels, got {len(pixels_16)}")

    # Compute mean color
    mr = sum(p[0] for p in pixels_16) / 16.0
    mg = sum(p[1] for p in pixels_16) / 16.0
    mb = sum(p[2] for p in pixels_16) / 16.0
    ma = sum(p[3] for p in pixels_16) / 16.0

    # Two-pass furthest-point principal axis (fast & exact for solid, 2-color, and ramps)
    far_a = pixels_16[0]
    max_d = -1.0
    for p in pixels_16:
        d = (p[0] - mr) ** 2 + (p[1] - mg) ** 2 + (p[2] - mb) ** 2 + (p[3] - ma) ** 2
        if d > max_d:
            max_d = d
            far_a = p

    far_b = far_a
    max_d = -1.0
    for p in pixels_16:
        d = (p[0] - far_a[0]) ** 2 + (p[1] - far_a[1]) ** 2 + (p[2] - far_a[2]) ** 2 + (p[3] - far_a[3]) ** 2
        if d > max_d:
            max_d = d
            far_b = p

    if max_d <= 0.0:
        # Flat solid block
        r7, g7, b7, a7, p0 = _quantize_mode6_endpoint(far_a[0], far_a[1], far_a[2], far_a[3])
        r0, g0, b0, a0 = r7, g7, b7, a7
        r1, g1, b1, a1, p1 = r7, g7, b7, a7, p0
        indices = [0] * 16
    else:
        ax_r = float(far_b[0] - far_a[0])
        ax_g = float(far_b[1] - far_a[1])
        ax_b = float(far_b[2] - far_a[2])
        ax_a = float(far_b[3] - far_a[3])
        inv_len2 = 1.0 / (ax_r * ax_r + ax_g * ax_g + ax_b * ax_b + ax_a * ax_a)

        # Project all 16 pixels onto axis to refine exact min/max endpoints
        t_min = 1e18
        t_max = -1e18
        for p in pixels_16:
            t = (
                (p[0] - far_a[0]) * ax_r
                + (p[1] - far_a[1]) * ax_g
                + (p[2] - far_a[2]) * ax_b
                + (p[3] - far_a[3]) * ax_a
            ) * inv_len2
            if t < t_min:
                t_min = t
            if t > t_max:
                t_max = t

        e0r = min(255.0, max(0.0, far_a[0] + t_min * ax_r))
        e0g = min(255.0, max(0.0, far_a[1] + t_min * ax_g))
        e0b = min(255.0, max(0.0, far_a[2] + t_min * ax_b))
        e0a = min(255.0, max(0.0, far_a[3] + t_min * ax_a))

        e1r = min(255.0, max(0.0, far_a[0] + t_max * ax_r))
        e1g = min(255.0, max(0.0, far_a[1] + t_max * ax_g))
        e1b = min(255.0, max(0.0, far_a[2] + t_max * ax_b))
        e1a = min(255.0, max(0.0, far_a[3] + t_max * ax_a))

        r0, g0, b0, a0, p0 = _quantize_mode6_endpoint(e0r, e0g, e0b, e0a)
        r1, g1, b1, a1, p1 = _quantize_mode6_endpoint(e1r, e1g, e1b, e1a)

        # Reconstruct exact 8-bit decoded endpoints and build 16-entry palette
        q0r, q0g, q0b, q0a = (r0 << 1) | p0, (g0 << 1) | p0, (b0 << 1) | p0, (a0 << 1) | p0
        q1r, q1g, q1b, q1a = (r1 << 1) | p1, (g1 << 1) | p1, (b1 << 1) | p1, (a1 << 1) | p1

        palette = [
            (
                ((64 - w) * q0r + w * q1r + 32) >> 6,
                ((64 - w) * q0g + w * q1g + 32) >> 6,
                ((64 - w) * q0b + w * q1b + 32) >> 6,
                ((64 - w) * q0a + w * q1a + 32) >> 6,
            )
            for w in BC7_WEIGHTS4
        ]

        indices = []
        for pr, pg, pb, pa in pixels_16:
            best_idx = 0
            best_err = 1 << 30
            for idx, (cr, cg, cb, ca) in enumerate(palette):
                dr = pr - cr
                dg = pg - cg
                db = pb - cb
                da = pa - ca
                err = dr * dr + dg * dg + db * db + da * da
                if err < best_err:
                    best_err = err
                    best_idx = idx
            indices.append(best_idx)

    # Enforce anchor index 0 MSB == 0 (if >= 8, swap endpoints and invert all indices)
    if indices[0] >= 8:
        r0, r1 = r1, r0
        g0, g1 = g1, g0
        b0, b1 = b1, b0
        a0, a1 = a1, a0
        p0, p1 = p1, p0
        indices = [15 - i for i in indices]

    assert indices[0] < 8

    # Pack 128 bits LSB-first
    bits = 1 << 6  # Mode 6: 0b1000000 (bits 0..6)
    pos = 7
    for val in (r0, r1, g0, g1, b0, b1, a0, a1):
        bits |= (val & 0x7F) << pos
        pos += 7
    bits |= (p0 & 1) << pos
    pos += 1
    bits |= (p1 & 1) << pos
    pos += 1
    # Anchor index 0 (3 bits)
    bits |= (indices[0] & 0x7) << pos
    pos += 3
    # Remaining indices 1..15 (4 bits each)
    for i in range(1, 16):
        bits |= (indices[i] & 0xF) << pos
        pos += 4
    assert pos == 128
    return bits.to_bytes(16, "little")


def decode_bc7_mode6_block(block: bytes) -> List[Tuple[int, int, int, int]]:
    """Decode a 16-byte BC7 Mode 6 block back into 16 RGBA8 pixels (for validation/tests)."""
    if len(block) != 16:
        raise ValueError(f"Expected 16-byte BC7 block, got {len(block)}")
    bits = int.from_bytes(block, "little")
    mode_bits = bits & 0x7F
    if mode_bits != 0x40:
        raise ValueError(f"Block is not BC7 Mode 6 (low 7 bits = 0b{mode_bits:07b})")

    pos = 7
    ep: List[int] = []
    for _ in range(8):
        ep.append((bits >> pos) & 0x7F)
        pos += 7
    r0, r1, g0, g1, b0, b1, a0, a1 = ep
    p0 = (bits >> pos) & 1
    pos += 1
    p1 = (bits >> pos) & 1
    pos += 1

    q0 = ((r0 << 1) | p0, (g0 << 1) | p0, (b0 << 1) | p0, (a0 << 1) | p0)
    q1 = ((r1 << 1) | p1, (g1 << 1) | p1, (b1 << 1) | p1, (a1 << 1) | p1)

    idx0 = (bits >> pos) & 0x7
    pos += 3
    indices = [idx0]
    for _ in range(15):
        indices.append((bits >> pos) & 0xF)
        pos += 4

    out: List[Tuple[int, int, int, int]] = []
    for idx in indices:
        w = BC7_WEIGHTS4[idx]
        iw = 64 - w
        out.append(
            (
                (iw * q0[0] + w * q1[0] + 32) >> 6,
                (iw * q0[1] + w * q1[1] + 32) >> 6,
                (iw * q0[2] + w * q1[2] + 32) >> 6,
                (iw * q0[3] + w * q1[3] + 32) >> 6,
            )
        )
    return out


def bc7_block_mode(block: bytes) -> int:
    """Return BC7 block mode (0..7), or 8 if reserved/invalid (byte 0 == 0x00)."""
    b0 = block[0]
    for m in range(8):
        if b0 & (1 << m):
            return m
    return 8


def _encode_bc7_mode6_numpy(image: RGBAImage) -> bytes:
    """Vectorized NumPy BC7 Mode 6 encoder (fast fallback when native C++ lib is absent)."""
    w, h = image.width, image.height
    bw = (w + 3) // 4
    bh = (h + 3) // 4
    pw, ph = bw * 4, bh * 4

    arr = np.frombuffer(image.data, dtype=np.uint8).reshape((h, w, 4))
    if pw != w or ph != h:
        arr = np.pad(arr, ((0, ph - h), (0, pw - w), (0, 0)), mode="edge")

    # Reshape into (N, 16, 4) float32 blocks
    blocks = arr.reshape((bh, 4, bw, 4, 4)).transpose(0, 2, 1, 3, 4).reshape((-1, 16, 4))
    n = blocks.shape[0]
    bf = blocks.astype(np.float32)

    # Furthest-point axis per block
    mean = bf.mean(axis=1, keepdims=True)
    d_mean = ((bf - mean) ** 2).sum(axis=2)
    idx_a = np.argmax(d_mean, axis=1)
    far_a = bf[np.arange(n), idx_a]  # (N, 4)

    d_a = ((bf - far_a[:, None, :]) ** 2).sum(axis=2)
    idx_b = np.argmax(d_a, axis=1)
    far_b = bf[np.arange(n), idx_b]  # (N, 4)

    axis = far_b - far_a  # (N, 4)
    len2 = (axis * axis).sum(axis=1)  # (N,)
    non_flat = len2 > 0.0
    safe_len2 = np.where(non_flat, len2, 1.0)

    proj = ((bf - far_a[:, None, :]) * axis[:, None, :]).sum(axis=2) / safe_len2[:, None]
    t_min = proj.min(axis=1)[:, None]
    t_max = proj.max(axis=1)[:, None]

    e0 = np.where(non_flat[:, None], np.clip(far_a + t_min * axis, 0.0, 255.0), far_a)
    e1 = np.where(non_flat[:, None], np.clip(far_a + t_max * axis, 0.0, 255.0), far_a)

    # Quantize endpoints to 7-bit + optimal shared p-bit per endpoint
    def _quant(ep: "np.ndarray") -> Tuple["np.ndarray", "np.ndarray"]:
        q0 = np.clip(np.rint(ep * 0.5), 0, 127).astype(np.int32)
        q1 = np.clip(np.rint((ep - 1.0) * 0.5), 0, 127).astype(np.int32)
        err0 = (((q0 << 1) - ep) ** 2).sum(axis=1)
        err1 = ((((q1 << 1) | 1) - ep) ** 2).sum(axis=1)
        use1 = err1 < err0
        q = np.where(use1[:, None], q1, q0)
        p = use1.astype(np.int32)
        return q, p

    ep0_7, p0 = _quant(e0)  # (N, 4), (N,)
    ep1_7, p1 = _quant(e1)  # (N, 4), (N,)

    # Reconstruct 8-bit endpoints and find optimal 4-bit index per pixel
    c0 = ((ep0_7 << 1) | p0[:, None]).astype(np.int32)  # (N, 4)
    c1 = ((ep1_7 << 1) | p1[:, None]).astype(np.int32)  # (N, 4)
    weights = np.array(BC7_WEIGHTS4, dtype=np.int32)[None, :, None]  # (1, 16, 1)
    pal = ((64 - weights) * c0[:, None, :] + weights * c1[:, None, :] + 32) >> 6  # (N, 16, 4)

    # Squared distance: (N, 16 pixels, 16 palette entries)
    diff = blocks.astype(np.int32)[:, :, None, :] - pal[:, None, :, :]
    dist = (diff * diff).sum(axis=3)
    indices = np.argmin(dist, axis=2).astype(np.int32)  # (N, 16)
    indices[~non_flat] = 0

    # Enforce anchor index 0 MSB == 0
    swap = indices[:, 0] >= 8
    if swap.any():
        ep0_new = np.where(swap[:, None], ep1_7, ep0_7)
        ep1_new = np.where(swap[:, None], ep0_7, ep1_7)
        p0_new = np.where(swap, p1, p0)
        p1_new = np.where(swap, p0, p1)
        ep0_7, ep1_7, p0, p1 = ep0_new, ep1_new, p0_new, p1_new
        indices[swap] = 15 - indices[swap]

    # Pack 128 bits as two uint64 (lo = bits 0..63, hi = bits 64..127)
    ep0_u = ep0_7.astype(np.uint64)
    ep1_u = ep1_7.astype(np.uint64)
    p0_u = p0.astype(np.uint64)
    p1_u = p1.astype(np.uint64)
    idx_u = indices.astype(np.uint64)

    lo = (
        np.uint64(1 << 6)
        | (ep0_u[:, 0] << np.uint64(7))
        | (ep1_u[:, 0] << np.uint64(14))
        | (ep0_u[:, 1] << np.uint64(21))
        | (ep1_u[:, 1] << np.uint64(28))
        | (ep0_u[:, 2] << np.uint64(35))
        | (ep1_u[:, 2] << np.uint64(42))
        | (ep0_u[:, 3] << np.uint64(49))
        | (ep1_u[:, 3] << np.uint64(56))
        | (p0_u << np.uint64(63))
    )
    hi = p1_u | ((idx_u[:, 0] & np.uint64(0x7)) << np.uint64(1))
    for i in range(1, 16):
        hi |= (idx_u[:, i] & np.uint64(0xF)) << np.uint64(4 + (i - 1) * 4)

    packed = np.empty((n, 2), dtype="<u8")
    packed[:, 0] = lo
    packed[:, 1] = hi
    return packed.tobytes()


def encode_bc7_python(image: RGBAImage, force_pure_python: bool = False) -> bytes:
    """Encode an RGBAImage into tightly packed BC7 Mode 6 blocks (zero external deps)."""
    if HAS_NUMPY and not force_pure_python:
        return _encode_bc7_mode6_numpy(image)

    w, h = image.width, image.height
    bw = (w + 3) // 4
    bh = (h + 3) // 4
    src = image.data
    out = bytearray(bw * bh * 16)
    dst_pos = 0

    for by in range(bh):
        for bx in range(bw):
            pixels: List[Tuple[int, int, int, int]] = []
            for py in range(4):
                sy = by * 4 + py
                if sy >= h:
                    sy = h - 1
                row = sy * w * 4
                for px in range(4):
                    sx = bx * 4 + px
                    if sx >= w:
                        sx = w - 1
                    si = row + sx * 4
                    pixels.append((src[si], src[si + 1], src[si + 2], src[si + 3]))
            out[dst_pos : dst_pos + 16] = encode_bc7_mode6_block(pixels)
            dst_pos += 16

    return bytes(out)


# ---------------------------------------------------------------------------
# Unified public encoder API
# ---------------------------------------------------------------------------


def resolve_bc7_backend(backend: str = "auto") -> str:
    """Resolve requested BC7 backend ('auto', 'native', 'cli', 'python')."""
    req = (backend or "auto").lower()
    if req == "auto":
        lib, _ = _load_native_bc7()
        if lib is not None:
            return "native"
        if find_bc7_cli() is not None:
            return "cli"
        return "python"
    if req in ("native", "cli", "python"):
        return req
    raise ValueError(f"Unknown BC7 backend '{backend}' (expected auto, native, cli, or python)")


def bc7_backend_summary() -> str:
    """Human-readable summary of the active BC7 compression backend."""
    lib, lib_path = _load_native_bc7()
    if lib is not None and lib_path is not None:
        return f"native C++ bc7enc ({lib_path.name})"
    cli = find_bc7_cli()
    if cli is not None:
        return f"CLI {cli[0]} ({cli[1]})"
    if HAS_NUMPY:
        return "Mode 6 BC7 (NumPy fallback)"
    return "Mode 6 BC7 (pure-Python fallback)"


def encode_bc7_surface(
    image: RGBAImage,
    quality: str = "fast",
    perceptual: bool = True,
    backend: str = "auto",
    num_threads: int = 0,
) -> bytes:
    """Encode a single mip surface into raw tightly packed BC7 blocks."""
    resolved = resolve_bc7_backend(backend)
    if resolved == "native":
        return encode_bc7_native(image, quality=quality, perceptual=perceptual, num_threads=num_threads)
    if resolved == "cli":
        try:
            return encode_bc7_cli(image, quality=quality)
        except Exception:
            return encode_bc7_python(image)
    return encode_bc7_python(image)


def encode_dds_bc7(
    image: RGBAImage,
    is_normal_map: bool = False,
    is_specular_map: bool = False,
    quality: str = "fast",
    backend: str = "auto",
    num_threads: int = 0,
) -> bytes:
    """Encode an RGBAImage + full pre-generated mipmap chain into DX10 BC7 DDS bytes."""
    if quality not in BC7_QUALITY_PARAMS:
        raise ValueError(f"Unknown BC7 quality '{quality}' (expected one of {BC7_QUALITY_CHOICES})")

    mips = generate_mipmaps(image, is_normal_map=is_normal_map, is_specular_map=is_specular_map)
    header = build_dds_bc7_header(
        width=image.width,
        height=image.height,
        mip_count=len(mips),
        dxgi_format=DXGI_FORMAT_BC7_UNORM,
    )
    perceptual = not (is_normal_map or is_specular_map)
    chunks: List[bytes] = [header]
    for mip in mips:
        chunks.append(
            encode_bc7_surface(
                mip,
                quality=quality,
                perceptual=perceptual,
                backend=backend,
                num_threads=num_threads,
            )
        )
    return b"".join(chunks)


def save_dds_bc7(
    image: RGBAImage,
    path: Union[str, Path],
    is_normal_map: bool = False,
    is_specular_map: bool = False,
    quality: str = "fast",
    backend: str = "auto",
    num_threads: int = 0,
) -> Path:
    """Encode and write a DX10 BC7 DDS file (creating parent directories as needed)."""
    out_path = Path(path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    dds_bytes = encode_dds_bc7(
        image,
        is_normal_map=is_normal_map,
        is_specular_map=is_specular_map,
        quality=quality,
        backend=backend,
        num_threads=num_threads,
    )
    out_path.write_bytes(dds_bytes)
    return out_path

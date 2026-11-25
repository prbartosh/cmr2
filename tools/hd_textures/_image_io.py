"""Self-contained RGBA image representation and PNG codec for OpenCMR2 HD textures.

Uses Pillow / NumPy automatically when installed for high throughput, and falls
back transparently to pure Python stdlib (zlib + struct) with zero external
dependencies.
"""

from __future__ import annotations

import struct
import zlib
from pathlib import Path
from typing import Tuple, Union

try:
    import numpy as np  # type: ignore

    HAS_NUMPY = True
except ImportError:
    np = None
    HAS_NUMPY = False

try:
    from PIL import Image as PILImage  # type: ignore

    HAS_PIL = True
except ImportError:
    PILImage = None
    HAS_PIL = False

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"


def backend_summary() -> str:
    """Return a human-readable string describing active acceleration backends."""
    parts = []
    if HAS_NUMPY:
        parts.append(f"numpy {getattr(np, '__version__', '?')}")
    if HAS_PIL:
        parts.append(f"Pillow {getattr(PILImage, '__version__', '?')}")
    if not parts:
        return "pure-python stdlib (install pillow + numpy for faster batch processing)"
    return " + ".join(parts)


class RGBAImage:
    """Simple 8-bit-per-channel RGBA image container (row-major, top-down)."""

    __slots__ = ("width", "height", "data")

    def __init__(self, width: int, height: int, data: Union[bytes, bytearray, memoryview]):
        if width <= 0 or height <= 0:
            raise ValueError(f"Invalid image dimensions: {width}x{height}")
        expected = width * height * 4
        buf = bytearray(data) if not isinstance(data, bytearray) else data
        if len(buf) != expected:
            raise ValueError(
                f"RGBA buffer length mismatch for {width}x{height}: expected {expected}, got {len(buf)}"
            )
        self.width = int(width)
        self.height = int(height)
        self.data = buf

    @classmethod
    def new(cls, width: int, height: int, color: Tuple[int, int, int, int] = (0, 0, 0, 255)) -> "RGBAImage":
        r, g, b, a = (c & 0xFF for c in color)
        pixel = bytes((r, g, b, a))
        return cls(width, height, bytearray(pixel * (width * height)))

    def copy(self) -> "RGBAImage":
        return RGBAImage(self.width, self.height, bytearray(self.data))

    def get_pixel(self, x: int, y: int) -> Tuple[int, int, int, int]:
        idx = (y * self.width + x) * 4
        d = self.data
        return (d[idx], d[idx + 1], d[idx + 2], d[idx + 3])

    def set_pixel(self, x: int, y: int, rgba: Tuple[int, int, int, int]) -> None:
        idx = (y * self.width + x) * 4
        d = self.data
        d[idx] = rgba[0] & 0xFF
        d[idx + 1] = rgba[1] & 0xFF
        d[idx + 2] = rgba[2] & 0xFF
        d[idx + 3] = rgba[3] & 0xFF

    def to_numpy(self):
        """Return an (H, W, 4) uint8 NumPy array copy (requires NumPy)."""
        if not HAS_NUMPY:
            raise RuntimeError("NumPy is not installed")
        return np.frombuffer(self.data, dtype=np.uint8).reshape((self.height, self.width, 4)).copy()

    @classmethod
    def from_numpy(cls, arr) -> "RGBAImage":
        if not HAS_NUMPY:
            raise RuntimeError("NumPy is not installed")
        arr_u8 = np.ascontiguousarray(np.clip(np.rint(arr), 0, 255), dtype=np.uint8)
        if arr_u8.ndim != 3 or arr_u8.shape[2] != 4:
            raise ValueError(f"Expected (H, W, 4) array, got {arr_u8.shape}")
        h, w, _ = arr_u8.shape
        return cls(w, h, bytearray(arr_u8.tobytes()))

    def to_pil(self):
        if not HAS_PIL:
            raise RuntimeError("Pillow is not installed")
        return PILImage.frombytes("RGBA", (self.width, self.height), bytes(self.data))

    @classmethod
    def from_pil(cls, img) -> "RGBAImage":
        rgba = img.convert("RGBA")
        return cls(rgba.width, rgba.height, bytearray(rgba.tobytes()))


def _png_chunk(chunk_type: bytes, payload: bytes) -> bytes:
    crc = zlib.crc32(chunk_type)
    crc = zlib.crc32(payload, crc) & 0xFFFFFFFF
    return struct.pack(">I", len(payload)) + chunk_type + payload + struct.pack(">I", crc)


def encode_png(image: RGBAImage, compress_level: int = 6) -> bytes:
    """Encode an RGBAImage to PNG bytes (uses Pillow if available, else stdlib zlib)."""
    if HAS_PIL:
        import io

        buf = io.BytesIO()
        image.to_pil().save(buf, format="PNG", compress_level=compress_level)
        return buf.getvalue()

    w, h = image.width, image.height
    stride = w * 4
    data = image.data
    raw = bytearray(h * (stride + 1))
    mv_data = memoryview(data)
    dst_pos = 0
    src_pos = 0
    for _ in range(h):
        raw[dst_pos] = 0  # Filter type 0 (None)
        raw[dst_pos + 1 : dst_pos + 1 + stride] = mv_data[src_pos : src_pos + stride]
        dst_pos += stride + 1
        src_pos += stride

    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    idat = zlib.compress(bytes(raw), level=compress_level)
    return PNG_SIGNATURE + _png_chunk(b"IHDR", ihdr) + _png_chunk(b"IDAT", idat) + _png_chunk(b"IEND", b"")


def _paeth_predictor(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png(raw_bytes: bytes) -> RGBAImage:
    """Decode PNG bytes into an RGBAImage (8-bit Gray/RGB/GrayA/RGBA, non-interlaced)."""
    if HAS_PIL:
        import io

        with PILImage.open(io.BytesIO(raw_bytes)) as im:
            return RGBAImage.from_pil(im)

    if len(raw_bytes) < 8 or raw_bytes[:8] != PNG_SIGNATURE:
        raise ValueError("Not a valid PNG file (bad signature)")

    pos = 8
    width = height = bit_depth = color_type = interlace = 0
    idat_parts = []
    palette = None
    trns = None

    while pos + 8 <= len(raw_bytes):
        length, ctype = struct.unpack_from(">I4s", raw_bytes, pos)
        pos += 8
        chunk_data = raw_bytes[pos : pos + length]
        pos += length + 4  # skip CRC
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, compression, flt, interlace = struct.unpack(">IIBBBBB", chunk_data)
            if compression != 0 or flt != 0:
                raise ValueError(f"Unsupported PNG compression/filter method: {compression}/{flt}")
            if interlace != 0:
                raise ValueError("Interlaced PNG is not supported in pure-Python fallback (install Pillow)")
            if bit_depth != 8:
                raise ValueError(f"Only 8-bit PNG is supported in pure-Python fallback (got {bit_depth}-bit)")
        elif ctype == b"PLTE":
            palette = chunk_data
        elif ctype == b"tRNS":
            trns = chunk_data
        elif ctype == b"IDAT":
            idat_parts.append(chunk_data)
        elif ctype == b"IEND":
            break

    if width <= 0 or height <= 0:
        raise ValueError("PNG missing IHDR chunk")

    channels_by_type = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
    if color_type not in channels_by_type:
        raise ValueError(f"Unsupported PNG color type: {color_type}")
    bpp = channels_by_type[color_type]
    stride = width * bpp

    decompressed = zlib.decompress(b"".join(idat_parts))
    expected_len = height * (stride + 1)
    if len(decompressed) < expected_len:
        raise ValueError(f"Truncated PNG image data: expected {expected_len}, got {len(decompressed)}")

    recon = bytearray(height * stride)
    prev_row = bytearray(stride)

    src = 0
    for y in range(height):
        flt = decompressed[src]
        src += 1
        row_start = y * stride
        cur = bytearray(decompressed[src : src + stride])
        src += stride
        if flt == 0:
            pass
        elif flt == 1:
            for i in range(bpp, stride):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xFF
        elif flt == 2:
            for i in range(stride):
                cur[i] = (cur[i] + prev_row[i]) & 0xFF
        elif flt == 3:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev_row[i]) >> 1)) & 0xFF
        elif flt == 4:
            for i in range(stride):
                left = cur[i - bpp] if i >= bpp else 0
                up = prev_row[i]
                up_left = prev_row[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + _paeth_predictor(left, up, up_left)) & 0xFF
        else:
            raise ValueError(f"Unsupported PNG scanline filter: {flt}")
        recon[row_start : row_start + stride] = cur
        prev_row = cur

    if color_type == 6:
        return RGBAImage(width, height, recon)

    out = bytearray(width * height * 4)
    if color_type == 2:
        si = 0
        di = 0
        total = width * height
        for _ in range(total):
            out[di] = recon[si]
            out[di + 1] = recon[si + 1]
            out[di + 2] = recon[si + 2]
            out[di + 3] = 255
            si += 3
            di += 4
    elif color_type == 0:
        for i, v in enumerate(recon):
            di = i * 4
            out[di] = v
            out[di + 1] = v
            out[di + 2] = v
            out[di + 3] = 255
    elif color_type == 4:
        si = 0
        di = 0
        total = width * height
        for _ in range(total):
            v = recon[si]
            a = recon[si + 1]
            out[di] = v
            out[di + 1] = v
            out[di + 2] = v
            out[di + 3] = a
            si += 2
            di += 4
    elif color_type == 3:
        if palette is None:
            raise ValueError("Indexed PNG missing PLTE chunk")
        for i, idx in enumerate(recon):
            di = i * 4
            pi = idx * 3
            out[di] = palette[pi]
            out[di + 1] = palette[pi + 1]
            out[di + 2] = palette[pi + 2]
            out[di + 3] = trns[idx] if (trns is not None and idx < len(trns)) else 255

    return RGBAImage(width, height, out)


def save_png(image: RGBAImage, path: Union[str, Path], compress_level: int = 6) -> Path:
    """Write an RGBAImage to a PNG file, creating parent directories if needed."""
    out_path = Path(path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_bytes(encode_png(image, compress_level=compress_level))
    return out_path


def load_png(path: Union[str, Path]) -> RGBAImage:
    """Load a PNG file from disk into an RGBAImage."""
    return decode_png(Path(path).read_bytes())

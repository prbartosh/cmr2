#!/usr/bin/env python3
"""Batch HD texture upscaling and PBR-lite material map generation for OpenCMR2.

Produces "Faithful+" 1K/2K textures (2x or 4x upscaling, capped at 2048px,
preserving exact aspect ratio and 1:1 UV alignment) along with paired material
maps for Stage 6 `texture_override.cpp`:
  - `<name>.png`   : Enhanced RGBA albedo/diffuse override
  - `<name>_n.png` : Tangent-space normal map, neutral (128, 128, 255)
  - `<name>_s.png` : Specular/reflection mask (R=specular, G=cubemap/Fresnel
                     reflection multiplier, B=clearcoat, A=255)

Material maps are NOT generated for `ui_skip` (skipped completely) or
`sky_backdrop` (albedo-only smooth upscale).
"""

from __future__ import annotations

import argparse
import math
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Tuple

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from _image_io import HAS_NUMPY, HAS_PIL, RGBAImage, load_png, save_png
    from classify_textures import CATEGORIES, ClassificationResult, classify_texture
else:
    from ._image_io import HAS_NUMPY, HAS_PIL, RGBAImage, load_png, save_png
    from .classify_textures import CATEGORIES, ClassificationResult, classify_texture

if HAS_NUMPY:
    import numpy as np  # type: ignore


@dataclass(frozen=True)
class CategoryProfile:
    """Category-specific upscaling, micro-detail, normal, and specular tuning."""

    unsharp_amount: float
    grain_amount: float
    normal_strength: float
    normal_fine_weight: float
    spec_base: float
    spec_luma_gain: float
    refl_base: float
    refl_luma_gain: float
    clearcoat_base: float
    clearcoat_gain: float
    generate_material_maps: bool


CATEGORY_PROFILES: Dict[str, CategoryProfile] = {
    "road_surface": CategoryProfile(
        unsharp_amount=0.35,
        grain_amount=5.5,
        normal_strength=2.6,
        normal_fine_weight=0.65,
        spec_base=42.0,
        spec_luma_gain=45.0,
        refl_base=24.0,
        refl_luma_gain=38.0,
        clearcoat_base=10.0,
        clearcoat_gain=20.0,
        generate_material_maps=True,
    ),
    "car_livery": CategoryProfile(
        unsharp_amount=0.22,
        grain_amount=0.0,
        normal_strength=0.75,
        normal_fine_weight=0.50,
        spec_base=178.0,
        spec_luma_gain=55.0,
        refl_base=172.0,
        refl_luma_gain=58.0,
        clearcoat_base=198.0,
        clearcoat_gain=48.0,
        generate_material_maps=True,
    ),
    "foliage_alpha": CategoryProfile(
        unsharp_amount=0.28,
        grain_amount=0.0,
        normal_strength=1.85,
        normal_fine_weight=0.60,
        spec_base=20.0,
        spec_luma_gain=24.0,
        refl_base=8.0,
        refl_luma_gain=16.0,
        clearcoat_base=0.0,
        clearcoat_gain=0.0,
        generate_material_maps=True,
    ),
    "buildings_props": CategoryProfile(
        unsharp_amount=0.32,
        grain_amount=2.5,
        normal_strength=3.1,
        normal_fine_weight=0.55,
        spec_base=30.0,
        spec_luma_gain=42.0,
        refl_base=16.0,
        refl_luma_gain=34.0,
        clearcoat_base=4.0,
        clearcoat_gain=14.0,
        generate_material_maps=True,
    ),
    "sky_backdrop": CategoryProfile(
        unsharp_amount=0.08,
        grain_amount=0.0,
        normal_strength=0.0,
        normal_fine_weight=0.50,
        spec_base=0.0,
        spec_luma_gain=0.0,
        refl_base=0.0,
        refl_luma_gain=0.0,
        clearcoat_base=0.0,
        clearcoat_gain=0.0,
        generate_material_maps=False,
    ),
}


@dataclass
class UpscaleResult:
    """Outputs generated for a single texture."""

    albedo: RGBAImage
    normal: Optional[RGBAImage]
    specular: Optional[RGBAImage]
    src_size: Tuple[int, int]
    dst_size: Tuple[int, int]
    category: str


def compute_target_size(
    width: int,
    height: int,
    scale: Optional[int] = None,
    max_size: int = 2048,
) -> Tuple[int, int]:
    """Choose target HD resolution (2x or 4x) capped at max_size, keeping aspect ratio."""
    max_dim = max(width, height)
    if scale is None:
        # Target 1K/2K: 4x for <= 256px textures, 2x for 512-1024px textures
        chosen = 4 if max_dim <= 256 else 2
    else:
        chosen = max(1, int(scale))

    out_w = width * chosen
    out_h = height * chosen
    if max(out_w, out_h) > max_size:
        # Prefer power-of-two halving when possible to preserve exact integer scale
        while chosen > 1 and max(width * chosen, height * chosen) > max_size:
            chosen = chosen // 2 if chosen >= 2 else 1
        out_w = width * chosen
        out_h = height * chosen
        if max(out_w, out_h) > max_size:
            ratio = max_size / float(max(width, height))
            out_w = max(1, int(round(width * ratio)))
            out_h = max(1, int(round(height * ratio)))
    return out_w, out_h


def pad_periodic(image: RGBAImage, pad_x: int, pad_y: int, wrap_x: bool = True, wrap_y: bool = True) -> RGBAImage:
    """Pad an image using periodic wrap-around (or edge clamp) on each axis."""
    w, h = image.width, image.height
    pw = w + 2 * pad_x
    ph = h + 2 * pad_y
    if HAS_NUMPY:
        arr = image.to_numpy()
        mode_y = "wrap" if wrap_y else "edge"
        mode_x = "wrap" if wrap_x else "edge"
        if mode_y == mode_x:
            padded = np.pad(arr, ((pad_y, pad_y), (pad_x, pad_x), (0, 0)), mode=mode_x)
        else:
            padded = np.pad(arr, ((pad_y, pad_y), (0, 0), (0, 0)), mode=mode_y)
            padded = np.pad(padded, ((0, 0), (pad_x, pad_x), (0, 0)), mode=mode_x)
        return RGBAImage.from_numpy(padded)

    src = image.data
    out = bytearray(pw * ph * 4)
    for dy in range(ph):
        sy = (dy - pad_y) % h if wrap_y else min(h - 1, max(0, dy - pad_y))
        src_row = sy * w * 4
        dst_row = dy * pw * 4
        for dx in range(pw):
            sx = (dx - pad_x) % w if wrap_x else min(w - 1, max(0, dx - pad_x))
            si = src_row + sx * 4
            di = dst_row + dx * 4
            out[di : di + 4] = src[si : si + 4]
    return RGBAImage(pw, ph, out)


def dilate_alpha_rgb(image: RGBAImage, alpha_threshold: int = 16, max_radius: int = 8) -> RGBAImage:
    """Bleed nearest opaque RGB values into transparent pixels to prevent alpha halos.

    Leaves the alpha channel completely unchanged. Only pixels with
    `alpha < alpha_threshold` have their RGB replaced by adjacent opaque colors.
    """
    w, h = image.width, image.height

    if HAS_NUMPY:
        arr = image.to_numpy().astype(np.float32)
        alpha = arr[:, :, 3]
        valid = alpha >= float(alpha_threshold)
        if valid.all() or not valid.any():
            return image.copy()

        rgb = arr[:, :, :3].copy()
        mean_rgb = rgb[valid].mean(axis=0)
        cur_valid = valid.copy()

        for _ in range(max_radius):
            if cur_valid.all():
                break
            sum_rgb = np.zeros_like(rgb)
            cnt = np.zeros((h, w), dtype=np.float32)
            v_f = cur_valid.astype(np.float32)
            # 8-connected neighbor accumulation
            padded_rgb = np.pad(rgb * v_f[:, :, None], ((1, 1), (1, 1), (0, 0)), mode="edge")
            padded_v = np.pad(v_f, ((1, 1), (1, 1)), mode="edge")
            for oy in (0, 1, 2):
                for ox in (0, 1, 2):
                    if oy == 1 and ox == 1:
                        continue
                    wgt = 1.0 if (oy == 1 or ox == 1) else 0.7071
                    sum_rgb += padded_rgb[oy : oy + h, ox : ox + w] * wgt
                    cnt += padded_v[oy : oy + h, ox : ox + w] * wgt
            fill_mask = (~cur_valid) & (cnt > 0.0)
            if not fill_mask.any():
                break
            rgb[fill_mask] = sum_rgb[fill_mask] / cnt[fill_mask, None]
            cur_valid |= fill_mask

        still_empty = ~cur_valid
        if still_empty.any():
            rgb[still_empty] = mean_rgb

        out = arr.copy()
        out[:, :, :3] = rgb
        return RGBAImage.from_numpy(out)

    # Pure-Python implementation (8-neighbor iterative flood dilation)
    src = image.data
    n = w * h
    valid = bytearray(n)
    r_buf = bytearray(n)
    g_buf = bytearray(n)
    b_buf = bytearray(n)
    a_buf = bytearray(n)

    sum_r = sum_g = sum_b = valid_count = 0
    for idx in range(n):
        p = idx * 4
        r, g, b, a = src[p], src[p + 1], src[p + 2], src[p + 3]
        r_buf[idx] = r
        g_buf[idx] = g
        b_buf[idx] = b
        a_buf[idx] = a
        if a >= alpha_threshold:
            valid[idx] = 1
            sum_r += r
            sum_g += g
            sum_b += b
            valid_count += 1

    if valid_count == 0 or valid_count == n:
        return image.copy()

    mean_r = sum_r // valid_count
    mean_g = sum_g // valid_count
    mean_b = sum_b // valid_count

    neighbors = ((-1, 0, 4), (1, 0, 4), (0, -1, 4), (0, 1, 4), (-1, -1, 3), (1, -1, 3), (-1, 1, 3), (1, 1, 3))
    for _ in range(max_radius):
        changed = []
        for y in range(h):
            row = y * w
            for x in range(w):
                idx = row + x
                if valid[idx]:
                    continue
                acc_r = acc_g = acc_b = acc_w = 0
                for dx, dy, wgt in neighbors:
                    nx = x + dx
                    ny = y + dy
                    if 0 <= nx < w and 0 <= ny < h:
                        nidx = ny * w + nx
                        if valid[nidx]:
                            acc_r += r_buf[nidx] * wgt
                            acc_g += g_buf[nidx] * wgt
                            acc_b += b_buf[nidx] * wgt
                            acc_w += wgt
                if acc_w > 0:
                    changed.append((idx, acc_r // acc_w, acc_g // acc_w, acc_b // acc_w))
        if not changed:
            break
        for idx, nr, ng, nb in changed:
            r_buf[idx] = nr
            g_buf[idx] = ng
            b_buf[idx] = nb
            valid[idx] = 1

    out = bytearray(n * 4)
    for idx in range(n):
        p = idx * 4
        if valid[idx]:
            out[p] = r_buf[idx]
            out[p + 1] = g_buf[idx]
            out[p + 2] = b_buf[idx]
        else:
            out[p] = mean_r
            out[p + 1] = mean_g
            out[p + 2] = mean_b
        out[p + 3] = a_buf[idx]
    return RGBAImage(w, h, out)


def _lanczos_kernel(x: float, a: float = 3.0) -> float:
    ax = abs(x)
    if ax < 1e-8:
        return 1.0
    if ax >= a:
        return 0.0
    pix = math.pi * x
    return (math.sin(pix) / pix) * (math.sin(pix / a) / (pix / a))


def _build_lanczos_table(
    src_len: int,
    dst_len: int,
    wrap: bool,
    a: float = 3.0,
) -> List[List[Tuple[int, float]]]:
    """Precompute 1D Lanczos-a tap (index, normalized_weight) lists."""
    scale = src_len / float(dst_len)
    support = a * max(1.0, scale)
    inv_filter_scale = 1.0 / max(1.0, scale)
    table: List[List[Tuple[int, float]]] = []
    for dst_i in range(dst_len):
        center = (dst_i + 0.5) * scale - 0.5
        start = int(math.floor(center - support))
        end = int(math.ceil(center + support))
        taps: List[Tuple[int, float]] = []
        w_sum = 0.0
        for s in range(start, end + 1):
            wgt = _lanczos_kernel((s - center) * inv_filter_scale, a=a)
            if wgt == 0.0:
                continue
            idx = (s % src_len) if wrap else min(src_len - 1, max(0, s))
            taps.append((idx, wgt))
            w_sum += wgt
        if w_sum == 0.0:
            idx = int(round(center))
            idx = (idx % src_len) if wrap else min(src_len - 1, max(0, idx))
            taps = [(idx, 1.0)]
        else:
            inv = 1.0 / w_sum
            # Coalesce duplicate wrapped/clamped indices for faster inner loops
            merged: Dict[int, float] = {}
            for idx, wgt in taps:
                merged[idx] = merged.get(idx, 0.0) + wgt * inv
            taps = list(merged.items())
        table.append(taps)
    return table


def _clamp_byte(v: float) -> int:
    iv = int(v + 0.5) if v >= 0.0 else 0
    if iv < 0:
        return 0
    if iv > 255:
        return 255
    return iv


def lanczos_resize(
    image: RGBAImage,
    out_w: int,
    out_h: int,
    wrap_x: bool = False,
    wrap_y: bool = False,
) -> RGBAImage:
    """Separable Lanczos-3 resize with exact periodic wrap support on either axis."""
    src_w, src_h = image.width, image.height
    if src_w == out_w and src_h == out_h:
        return image.copy()

    # Fast Pillow path using periodic padding so tile borders wrap cleanly
    if HAS_PIL:
        from PIL import Image as PILImage  # type: ignore

        pad_x = min(src_w, 8) if wrap_x else 0
        pad_y = min(src_h, 8) if wrap_y else 0
        work = pad_periodic(image, pad_x, pad_y, wrap_x=wrap_x, wrap_y=wrap_y) if (pad_x or pad_y) else image
        scale_x = out_w / float(src_w)
        scale_y = out_h / float(src_h)
        pw_out = int(round(work.width * scale_x))
        ph_out = int(round(work.height * scale_y))
        pil_res = work.to_pil().resize((pw_out, ph_out), resample=PILImage.Resampling.LANCZOS)
        if pad_x or pad_y:
            crop_x = int(round(pad_x * scale_x))
            crop_y = int(round(pad_y * scale_y))
            pil_res = pil_res.crop((crop_x, crop_y, crop_x + out_w, crop_y + out_h))
        return RGBAImage.from_pil(pil_res)

    x_table = _build_lanczos_table(src_w, out_w, wrap=wrap_x, a=3.0)
    y_table = _build_lanczos_table(src_h, out_h, wrap=wrap_y, a=3.0)

    if HAS_NUMPY:
        arr = image.to_numpy().astype(np.float32)
        # Horizontal pass
        tmp = np.zeros((src_h, out_w, 4), dtype=np.float32)
        for dx, taps in enumerate(x_table):
            acc = np.zeros((src_h, 4), dtype=np.float32)
            for sx, wgt in taps:
                acc += arr[:, sx, :] * wgt
            tmp[:, dx, :] = acc
        # Vertical pass
        out = np.zeros((out_h, out_w, 4), dtype=np.float32)
        for dy, taps in enumerate(y_table):
            acc = np.zeros((out_w, 4), dtype=np.float32)
            for sy, wgt in taps:
                acc += tmp[sy, :, :] * wgt
            out[dy, :, :] = acc
        return RGBAImage.from_numpy(out)

    # Pure-Python separable pass
    src = image.data
    tmp_r = [0.0] * (src_h * out_w)
    tmp_g = [0.0] * (src_h * out_w)
    tmp_b = [0.0] * (src_h * out_w)
    tmp_a = [0.0] * (src_h * out_w)

    for sy in range(src_h):
        src_row = sy * src_w * 4
        dst_row = sy * out_w
        for dx in range(out_w):
            ar = ag = ab = aa = 0.0
            for sx, wgt in x_table[dx]:
                si = src_row + sx * 4
                ar += src[si] * wgt
                ag += src[si + 1] * wgt
                ab += src[si + 2] * wgt
                aa += src[si + 3] * wgt
            di = dst_row + dx
            tmp_r[di] = ar
            tmp_g[di] = ag
            tmp_b[di] = ab
            tmp_a[di] = aa

    out = bytearray(out_w * out_h * 4)
    for dy in range(out_h):
        dst_row = dy * out_w * 4
        taps_y = y_table[dy]
        for dx in range(out_w):
            ar = ag = ab = aa = 0.0
            for sy, wgt in taps_y:
                ti = sy * out_w + dx
                ar += tmp_r[ti] * wgt
                ag += tmp_g[ti] * wgt
                ab += tmp_b[ti] * wgt
                aa += tmp_a[ti] * wgt
            di = dst_row + dx * 4
            out[di] = _clamp_byte(ar)
            out[di + 1] = _clamp_byte(ag)
            out[di + 2] = _clamp_byte(ab)
            out[di + 3] = _clamp_byte(aa)
    return RGBAImage(out_w, out_h, out)


def _deterministic_grain(x: int, y: int, w: int, h: int, wrap_x: bool, wrap_y: bool) -> float:
    """Periodic-safe zero-mean procedural micro-grain in [-1.0, 1.0]."""
    px = x % w if wrap_x else x
    py = y % h if wrap_y else y
    # Mix two spatial frequencies for natural asphalt/stone micro-texture
    h1 = ((px * 374761393 + py * 668265263) ^ ((px * 1274126177) >> 13)) & 0xFFFFFFFF
    h1 = ((h1 ^ (h1 >> 15)) * 2246822519) & 0xFFFFFFFF
    n1 = ((h1 & 0xFFFF) / 32767.5) - 1.0

    px2 = (px >> 1) % max(1, w >> 1) if wrap_x else (px >> 1)
    py2 = (py >> 1) % max(1, h >> 1) if wrap_y else (py >> 1)
    h2 = ((px2 * 2654435761 + py2 * 3266489917) ^ ((py2 * 668265263) >> 11)) & 0xFFFFFFFF
    h2 = ((h2 ^ (h2 >> 16)) * 668265263) & 0xFFFFFFFF
    n2 = ((h2 & 0xFFFF) / 32767.5) - 1.0

    return 0.65 * n1 + 0.35 * n2


def _sharpen_and_detail(
    image: RGBAImage,
    unsharp_amount: float,
    grain_amount: float,
    wrap_x: bool,
    wrap_y: bool,
    preserve_alpha_cutout: bool,
) -> RGBAImage:
    """Apply faithful unsharp local contrast, subtle surface micro-grain, and crisp alpha."""
    w, h = image.width, image.height

    if HAS_NUMPY:
        arr = image.to_numpy().astype(np.float32)
        rgb = arr[:, :, :3]
        alpha = arr[:, :, 3]

        if unsharp_amount > 0.0:
            mode_y = "wrap" if wrap_y else "edge"
            mode_x = "wrap" if wrap_x else "edge"
            # Separable [1, 2, 1]/4 Gaussian blur
            if mode_x == "wrap":
                blur_x = 0.25 * np.roll(rgb, 1, axis=1) + 0.5 * rgb + 0.25 * np.roll(rgb, -1, axis=1)
            else:
                p = np.pad(rgb, ((0, 0), (1, 1), (0, 0)), mode="edge")
                blur_x = 0.25 * p[:, :-2, :] + 0.5 * p[:, 1:-1, :] + 0.25 * p[:, 2:, :]
            if mode_y == "wrap":
                blur = 0.25 * np.roll(blur_x, 1, axis=0) + 0.5 * blur_x + 0.25 * np.roll(blur_x, -1, axis=0)
            else:
                p = np.pad(blur_x, ((1, 1), (0, 0), (0, 0)), mode="edge")
                blur = 0.25 * p[:-2, :, :] + 0.5 * p[1:-1, :, :] + 0.25 * p[2:, :, :]

            detail = rgb - blur
            if preserve_alpha_cutout:
                # Attenuate sharpening on transparent pixels to avoid edge ringing
                weight = np.clip((alpha - 24.0) / 160.0, 0.0, 1.0)[:, :, None]
                rgb = rgb + detail * (unsharp_amount * weight)
            else:
                rgb = rgb + detail * unsharp_amount

        if grain_amount > 0.0:
            ys = np.arange(h, dtype=np.uint32)[:, None]
            xs = np.arange(w, dtype=np.uint32)[None, :]
            h1 = ((xs * np.uint32(374761393) + ys * np.uint32(668265263)) ^ ((xs * np.uint32(1274126177)) >> np.uint32(13)))
            h1 = (h1 ^ (h1 >> np.uint32(15))) * np.uint32(2246822519)
            n1 = ((h1 & np.uint32(0xFFFF)).astype(np.float32) / 32767.5) - 1.0
            xs2 = (xs >> np.uint32(1)) % np.uint32(max(1, w >> 1)) if wrap_x else (xs >> np.uint32(1))
            ys2 = (ys >> np.uint32(1)) % np.uint32(max(1, h >> 1)) if wrap_y else (ys >> np.uint32(1))
            h2 = ((xs2 * np.uint32(2654435761) + ys2 * np.uint32(3266489917)) ^ ((ys2 * np.uint32(668265263)) >> np.uint32(11)))
            h2 = (h2 ^ (h2 >> np.uint32(16))) * np.uint32(668265263)
            n2 = ((h2 & np.uint32(0xFFFF)).astype(np.float32) / 32767.5) - 1.0
            noise = (0.65 * n1 + 0.35 * n2) * grain_amount
            rgb = rgb + noise[:, :, None]

        if preserve_alpha_cutout:
            # Gentle alpha edge contrast preservation (keeps antialiasing crisp)
            a_norm = alpha / 255.0
            a_sharp = np.clip((a_norm - 0.5) * 1.18 + 0.5, 0.0, 1.0)
            alpha = a_sharp * 255.0

        out = np.empty_like(arr)
        out[:, :, :3] = np.clip(rgb, 0.0, 255.0)
        out[:, :, 3] = np.clip(alpha, 0.0, 255.0)
        return RGBAImage.from_numpy(out)

    # Pure-Python path
    src = image.data
    n = w * h
    if unsharp_amount > 0.0:
        bx_r = [0.0] * n
        bx_g = [0.0] * n
        bx_b = [0.0] * n
        for y in range(h):
            row_p = y * w * 4
            row_i = y * w
            for x in range(w):
                xl = (x - 1) % w if wrap_x else (x - 1 if x > 0 else 0)
                xr = (x + 1) % w if wrap_x else (x + 1 if x + 1 < w else w - 1)
                il = row_p + xl * 4
                ic = row_p + x * 4
                ir = row_p + xr * 4
                di = row_i + x
                bx_r[di] = 0.25 * src[il] + 0.5 * src[ic] + 0.25 * src[ir]
                bx_g[di] = 0.25 * src[il + 1] + 0.5 * src[ic + 1] + 0.25 * src[ir + 1]
                bx_b[di] = 0.25 * src[il + 2] + 0.5 * src[ic + 2] + 0.25 * src[ir + 2]
    else:
        bx_r = bx_g = bx_b = []

    out = bytearray(n * 4)
    for y in range(h):
        yu = (y - 1) % h if wrap_y else (y - 1 if y > 0 else 0)
        yd = (y + 1) % h if wrap_y else (y + 1 if y + 1 < h else h - 1)
        row_u = yu * w
        row_c = y * w
        row_d = yd * w
        for x in range(w):
            ci = (row_c + x) * 4
            r = float(src[ci])
            g = float(src[ci + 1])
            b = float(src[ci + 2])
            a = float(src[ci + 3])

            if unsharp_amount > 0.0:
                br = 0.25 * bx_r[row_u + x] + 0.5 * bx_r[row_c + x] + 0.25 * bx_r[row_d + x]
                bg = 0.25 * bx_g[row_u + x] + 0.5 * bx_g[row_c + x] + 0.25 * bx_g[row_d + x]
                bb = 0.25 * bx_b[row_u + x] + 0.5 * bx_b[row_c + x] + 0.25 * bx_b[row_d + x]
                amt = unsharp_amount
                if preserve_alpha_cutout:
                    wgt = min(1.0, max(0.0, (a - 24.0) / 160.0))
                    amt *= wgt
                r += (r - br) * amt
                g += (g - bg) * amt
                b += (b - bb) * amt

            if grain_amount > 0.0:
                delta = _deterministic_grain(x, y, w, h, wrap_x, wrap_y) * grain_amount
                r += delta
                g += delta
                b += delta

            if preserve_alpha_cutout:
                a_norm = (a / 255.0 - 0.5) * 1.18 + 0.5
                a = min(1.0, max(0.0, a_norm)) * 255.0

            out[ci] = _clamp_byte(r)
            out[ci + 1] = _clamp_byte(g)
            out[ci + 2] = _clamp_byte(b)
            out[ci + 3] = _clamp_byte(a)

    return RGBAImage(w, h, out)


def generate_normal_map(
    image: RGBAImage,
    category: str = "buildings_props",
    wrap_x: bool = False,
    wrap_y: bool = False,
    strength_override: Optional[float] = None,
) -> RGBAImage:
    """Generate a tangent-space normal map from multi-scale Scharr luminance gradients.

    Neutral flat normal is encoded as (128, 128, 255, 255).
    """
    profile = CATEGORY_PROFILES.get(category, CATEGORY_PROFILES["buildings_props"])
    strength = profile.normal_strength if strength_override is None else float(strength_override)
    fine_w = profile.normal_fine_weight
    coarse_w = 1.0 - fine_w
    w, h = image.width, image.height

    if HAS_NUMPY:
        arr = image.to_numpy().astype(np.float32)
        luma = (0.299 * arr[:, :, 0] + 0.587 * arr[:, :, 1] + 0.114 * arr[:, :, 2]) / 255.0
        alpha = arr[:, :, 3] / 255.0

        def scharr(field):
            p = np.pad(
                np.pad(field, ((1, 1), (0, 0)), mode="wrap" if wrap_y else "edge"),
                ((0, 0), (1, 1)),
                mode="wrap" if wrap_x else "edge",
            )
            gx = (
                -3.0 * p[:-2, :-2]
                + 3.0 * p[:-2, 2:]
                - 10.0 * p[1:-1, :-2]
                + 10.0 * p[1:-1, 2:]
                - 3.0 * p[2:, :-2]
                + 3.0 * p[2:, 2:]
            ) / 32.0
            gy = (
                -3.0 * p[:-2, :-2]
                - 10.0 * p[:-2, 1:-1]
                - 3.0 * p[:-2, 2:]
                + 3.0 * p[2:, :-2]
                + 10.0 * p[2:, 1:-1]
                + 3.0 * p[2:, 2:]
            ) / 32.0
            return gx, gy

        # Coarse luminance via separable 3x3 blur
        p_l = np.pad(
            np.pad(luma, ((1, 1), (0, 0)), mode="wrap" if wrap_y else "edge"),
            ((0, 0), (1, 1)),
            mode="wrap" if wrap_x else "edge",
        )
        coarse_l = (
            p_l[:-2, :-2]
            + 2.0 * p_l[:-2, 1:-1]
            + p_l[:-2, 2:]
            + 2.0 * p_l[1:-1, :-2]
            + 4.0 * p_l[1:-1, 1:-1]
            + 2.0 * p_l[1:-1, 2:]
            + p_l[2:, :-2]
            + 2.0 * p_l[2:, 1:-1]
            + p_l[2:, 2:]
        ) / 16.0

        gx_f, gy_f = scharr(luma)
        gx_c, gy_c = scharr(coarse_l)
        gx = (fine_w * gx_f + coarse_w * gx_c) * strength
        gy = (fine_w * gy_f + coarse_w * gy_c) * strength

        if category == "foliage_alpha":
            mask = np.clip((alpha - 0.1) / 0.4, 0.0, 1.0)
            gx *= mask
            gy *= mask

        nx = -gx
        ny = -gy
        nz = np.ones_like(nx)
        inv_len = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
        nx *= inv_len
        ny *= inv_len
        nz *= inv_len

        out = np.empty((h, w, 4), dtype=np.float32)
        out[:, :, 0] = np.floor((nx * 0.5 + 0.5) * 255.0 + 0.5)
        out[:, :, 1] = np.floor((ny * 0.5 + 0.5) * 255.0 + 0.5)
        out[:, :, 2] = np.floor((nz * 0.5 + 0.5) * 255.0 + 0.5)
        out[:, :, 3] = 255.0
        return RGBAImage.from_numpy(out)

    # Pure-Python multi-scale Scharr normal map
    src = image.data
    n = w * h
    luma = [0.0] * n
    for i in range(n):
        p = i * 4
        luma[i] = (0.299 * src[p] + 0.587 * src[p + 1] + 0.114 * src[p + 2]) / 255.0

    # Precompute index lookups for wrap vs clamp
    xm1 = [(x - 1) % w if wrap_x else (x - 1 if x > 0 else 0) for x in range(w)]
    xp1 = [(x + 1) % w if wrap_x else (x + 1 if x + 1 < w else w - 1) for x in range(w)]
    ym1 = [((y - 1) % h if wrap_y else (y - 1 if y > 0 else 0)) * w for y in range(h)]
    yp1 = [((y + 1) % h if wrap_y else (y + 1 if y + 1 < h else h - 1)) * w for y in range(h)]

    # Coarse blurred luminance
    coarse = [0.0] * n
    for y in range(h):
        ru = ym1[y]
        rc = y * w
        rd = yp1[y]
        for x in range(w):
            xl = xm1[x]
            xr = xp1[x]
            coarse[rc + x] = (
                luma[ru + xl]
                + 2.0 * luma[ru + x]
                + luma[ru + xr]
                + 2.0 * luma[rc + xl]
                + 4.0 * luma[rc + x]
                + 2.0 * luma[rc + xr]
                + luma[rd + xl]
                + 2.0 * luma[rd + x]
                + luma[rd + xr]
            ) * 0.0625

    out = bytearray(n * 4)
    is_foliage = category == "foliage_alpha"
    inv32 = 1.0 / 32.0
    for y in range(h):
        ru = ym1[y]
        rc = y * w
        rd = yp1[y]
        for x in range(w):
            xl = xm1[x]
            xr = xp1[x]

            gxf = (
                -3.0 * luma[ru + xl]
                + 3.0 * luma[ru + xr]
                - 10.0 * luma[rc + xl]
                + 10.0 * luma[rc + xr]
                - 3.0 * luma[rd + xl]
                + 3.0 * luma[rd + xr]
            ) * inv32
            gyf = (
                -3.0 * luma[ru + xl]
                - 10.0 * luma[ru + x]
                - 3.0 * luma[ru + xr]
                + 3.0 * luma[rd + xl]
                + 10.0 * luma[rd + x]
                + 3.0 * luma[rd + xr]
            ) * inv32

            gxc = (
                -3.0 * coarse[ru + xl]
                + 3.0 * coarse[ru + xr]
                - 10.0 * coarse[rc + xl]
                + 10.0 * coarse[rc + xr]
                - 3.0 * coarse[rd + xl]
                + 3.0 * coarse[rd + xr]
            ) * inv32
            gyc = (
                -3.0 * coarse[ru + xl]
                - 10.0 * coarse[ru + x]
                - 3.0 * coarse[ru + xr]
                + 3.0 * coarse[rd + xl]
                + 10.0 * coarse[rd + x]
                + 3.0 * coarse[rd + xr]
            ) * inv32

            gx = (fine_w * gxf + coarse_w * gxc) * strength
            gy = (fine_w * gyf + coarse_w * gyc) * strength

            di = (rc + x) * 4
            if is_foliage:
                a_norm = src[di + 3] / 255.0
                mask = min(1.0, max(0.0, (a_norm - 0.1) / 0.4))
                gx *= mask
                gy *= mask

            nx = -gx
            ny = -gy
            inv_len = 1.0 / math.sqrt(nx * nx + ny * ny + 1.0)
            nx *= inv_len
            ny *= inv_len
            nz = inv_len

            out[di] = _clamp_byte((nx * 0.5 + 0.5) * 255.0)
            out[di + 1] = _clamp_byte((ny * 0.5 + 0.5) * 255.0)
            out[di + 2] = _clamp_byte((nz * 0.5 + 0.5) * 255.0)
            out[di + 3] = 255

    return RGBAImage(w, h, out)


def generate_specular_map(
    image: RGBAImage,
    category: str = "buildings_props",
) -> RGBAImage:
    """Generate a specular/reflection mask RGBA image tuned per category.

    Channel layout:
      R = specular strength [0..255]
      G = cubemap / Fresnel reflection multiplier [0..255]
      B = clearcoat / gloss accent [0..255]
      A = 255
    """
    profile = CATEGORY_PROFILES.get(category, CATEGORY_PROFILES["buildings_props"])
    w, h = image.width, image.height

    if HAS_NUMPY:
        arr = image.to_numpy().astype(np.float32)
        r = arr[:, :, 0] / 255.0
        g = arr[:, :, 1] / 255.0
        b = arr[:, :, 2] / 255.0
        a = arr[:, :, 3] / 255.0

        luma = 0.299 * r + 0.587 * g + 0.114 * b
        cmax = np.maximum(np.maximum(r, g), b)
        cmin = np.minimum(np.minimum(r, g), b)
        sat = np.where(cmax > 1e-4, (cmax - cmin) / np.maximum(cmax, 1e-4), 0.0)

        if category == "car_livery":
            # Glossy paint (saturated or bright) and dark reflective glass both reflect strongly
            glass_boost = np.clip((0.28 - luma) / 0.28, 0.0, 1.0) * (1.0 - sat) * 36.0
            paint_boost = sat * 24.0
            spec = profile.spec_base + luma * profile.spec_luma_gain + paint_boost + glass_boost * 0.5
            refl = profile.refl_base + luma * profile.refl_luma_gain + paint_boost * 0.8 + glass_boost
            coat = profile.clearcoat_base + (0.5 * luma + 0.5 * sat) * profile.clearcoat_gain
        elif category == "road_surface":
            # Slightly higher sheen on darker smooth asphalt and bright icy patches
            ice_boost = np.clip((luma - 0.65) / 0.35, 0.0, 1.0) * (1.0 - sat) * 55.0
            spec = profile.spec_base + (1.0 - sat * 0.5) * luma * profile.spec_luma_gain + ice_boost
            refl = profile.refl_base + (1.0 - sat * 0.6) * luma * profile.refl_luma_gain + ice_boost * 0.8
            coat = profile.clearcoat_base + ice_boost * 0.6
        elif category == "foliage_alpha":
            mask = np.clip((a - 0.06) / 0.25, 0.0, 1.0)
            spec = (profile.spec_base + luma * profile.spec_luma_gain) * mask
            refl = (profile.refl_base + luma * profile.refl_luma_gain) * mask
            coat = np.zeros_like(luma)
        else:
            # buildings_props: matte walls with mild sheen on bright metallic/glass areas
            spec = profile.spec_base + luma * profile.spec_luma_gain
            refl = profile.refl_base + luma * (1.0 - sat * 0.4) * profile.refl_luma_gain
            coat = profile.clearcoat_base + np.clip((luma - 0.5) * 2.0, 0.0, 1.0) * profile.clearcoat_gain

        out = np.empty((h, w, 4), dtype=np.float32)
        out[:, :, 0] = np.clip(spec, 0.0, 255.0)
        out[:, :, 1] = np.clip(refl, 0.0, 255.0)
        out[:, :, 2] = np.clip(coat, 0.0, 255.0)
        out[:, :, 3] = 255.0
        return RGBAImage.from_numpy(out)

    # Pure-Python path
    src = image.data
    n = w * h
    out = bytearray(n * 4)
    inv255 = 1.0 / 255.0

    for i in range(n):
        p = i * 4
        r = src[p] * inv255
        g = src[p + 1] * inv255
        b = src[p + 2] * inv255
        a = src[p + 3] * inv255

        luma = 0.299 * r + 0.587 * g + 0.114 * b
        cmax = r if r >= g else g
        if b > cmax:
            cmax = b
        cmin = r if r <= g else g
        if b < cmin:
            cmin = b
        sat = ((cmax - cmin) / cmax) if cmax > 1e-4 else 0.0

        if category == "car_livery":
            glass_boost = (max(0.0, min(1.0, (0.28 - luma) / 0.28))) * (1.0 - sat) * 36.0
            paint_boost = sat * 24.0
            spec = profile.spec_base + luma * profile.spec_luma_gain + paint_boost + glass_boost * 0.5
            refl = profile.refl_base + luma * profile.refl_luma_gain + paint_boost * 0.8 + glass_boost
            coat = profile.clearcoat_base + (0.5 * luma + 0.5 * sat) * profile.clearcoat_gain
        elif category == "road_surface":
            ice_boost = max(0.0, min(1.0, (luma - 0.65) / 0.35)) * (1.0 - sat) * 55.0
            spec = profile.spec_base + (1.0 - sat * 0.5) * luma * profile.spec_luma_gain + ice_boost
            refl = profile.refl_base + (1.0 - sat * 0.6) * luma * profile.refl_luma_gain + ice_boost * 0.8
            coat = profile.clearcoat_base + ice_boost * 0.6
        elif category == "foliage_alpha":
            mask = max(0.0, min(1.0, (a - 0.06) / 0.25))
            spec = (profile.spec_base + luma * profile.spec_luma_gain) * mask
            refl = (profile.refl_base + luma * profile.refl_luma_gain) * mask
            coat = 0.0
        else:
            spec = profile.spec_base + luma * profile.spec_luma_gain
            refl = profile.refl_base + luma * (1.0 - sat * 0.4) * profile.refl_luma_gain
            coat = profile.clearcoat_base + max(0.0, min(1.0, (luma - 0.5) * 2.0)) * profile.clearcoat_gain

        out[p] = _clamp_byte(spec)
        out[p + 1] = _clamp_byte(refl)
        out[p + 2] = _clamp_byte(coat)
        out[p + 3] = 255

    return RGBAImage(w, h, out)


def process_texture(
    image: RGBAImage,
    category: str,
    seamless: Optional[bool] = None,
    has_alpha_cutout: Optional[bool] = None,
    scale: Optional[int] = None,
    max_size: int = 2048,
) -> Optional[UpscaleResult]:
    """Run the complete HD enhancement + material-map generation for one texture.

    Returns None when category is `ui_skip`.
    """
    if category == "ui_skip":
        return None
    if category not in CATEGORY_PROFILES:
        raise ValueError(f"Unknown texture category: '{category}' (expected one of {CATEGORIES})")

    profile = CATEGORY_PROFILES[category]
    is_seamless = (category == "road_surface") if seamless is None else bool(seamless)
    wrap_x = is_seamless or (category == "sky_backdrop")
    wrap_y = is_seamless
    is_alpha_cutout = (category == "foliage_alpha") if has_alpha_cutout is None else bool(has_alpha_cutout)

    # 1. Color bleeding before upscaling on alpha-cutout textures to eliminate dark/white fringes
    pre = dilate_alpha_rgb(image) if is_alpha_cutout else image

    # 2. Target resolution (2x or 4x, capped at max_size, exact aspect ratio)
    out_w, out_h = compute_target_size(pre.width, pre.height, scale=scale, max_size=max_size)

    # 3. Detail-preserving Lanczos-3 upscale with periodic wrap for seamless tiles
    upscaled = lanczos_resize(pre, out_w, out_h, wrap_x=wrap_x, wrap_y=wrap_y)

    # 4. Faithful+ local contrast & subtle category-appropriate micro-grain
    albedo = _sharpen_and_detail(
        upscaled,
        unsharp_amount=profile.unsharp_amount,
        grain_amount=profile.grain_amount,
        wrap_x=wrap_x,
        wrap_y=wrap_y,
        preserve_alpha_cutout=is_alpha_cutout,
    )

    # 5. Material maps (skip for sky_backdrop & ui_skip)
    normal_map: Optional[RGBAImage] = None
    specular_map: Optional[RGBAImage] = None
    if profile.generate_material_maps:
        normal_map = generate_normal_map(albedo, category=category, wrap_x=wrap_x, wrap_y=wrap_y)
        specular_map = generate_specular_map(albedo, category=category)

    return UpscaleResult(
        albedo=albedo,
        normal=normal_map,
        specular=specular_map,
        src_size=(image.width, image.height),
        dst_size=(out_w, out_h),
        category=category,
    )


def save_upscale_result(result: UpscaleResult, output_dir: Path, key: str) -> Dict[str, str]:
    """Save `<key>.png`, and (when present) `<key>_n.png` and `<key>_s.png`."""
    output_dir.mkdir(parents=True, exist_ok=True)
    written: Dict[str, str] = {}
    written["albedo"] = str(save_png(result.albedo, output_dir / f"{key}.png"))
    if result.normal is not None:
        written["normal"] = str(save_png(result.normal, output_dir / f"{key}_n.png"))
    if result.specular is not None:
        written["specular"] = str(save_png(result.specular, output_dir / f"{key}_s.png"))
    return written


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Upscale a single PNG texture or directory of PNGs and generate _n/_s maps."
    )
    parser.add_argument("input", help="Input PNG file or directory of PNGs")
    parser.add_argument("-o", "--output", required=True, help="Output overrides directory")
    parser.add_argument(
        "--category",
        choices=CATEGORIES,
        default=None,
        help="Force category instead of auto-classifying each texture",
    )
    parser.add_argument("--scale", type=int, default=None, help="Upscale factor (default: auto 2x/4x)")
    parser.add_argument("--max-size", type=int, default=2048, help="Max output dimension in pixels (default: 2048)")
    args = parser.parse_args(list(argv) if argv is not None else None)

    in_path = Path(args.input)
    out_dir = Path(args.output)
    pngs = [in_path] if in_path.is_file() else sorted(in_path.rglob("*.png"))

    processed = 0
    skipped = 0
    for png in pngs:
        img = load_png(png)
        if args.category is None:
            cls: ClassificationResult = classify_texture(png.name, str(png.parent), image=img)
            cat = cls.category
            seamless = cls.seamless
            alpha_cutout = cls.has_alpha_cutout
            key = cls.key
        else:
            cat = args.category
            seamless = None
            alpha_cutout = None
            key = png.stem.lower()

        res = process_texture(
            img,
            category=cat,
            seamless=seamless,
            has_alpha_cutout=alpha_cutout,
            scale=args.scale,
            max_size=args.max_size,
        )
        if res is None:
            skipped += 1
            print(f"[skip] {key} ({cat})")
            continue
        written = save_upscale_result(res, out_dir, key)
        processed += 1
        maps = "+".join(written.keys())
        print(f"[ok]   {key} ({cat}) {res.src_size[0]}x{res.src_size[1]} -> {res.dst_size[0]}x{res.dst_size[1]} [{maps}]")

    print(f"Done: {processed} processed, {skipped} skipped.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

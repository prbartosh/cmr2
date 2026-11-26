#!/usr/bin/env python3
"""Neural normal map generation (DeepBump256 ONNX + Hybrid Scharr) and
Stage 10 PBR Roughness / Specular / Reflection / Clearcoat map generation.

Normal map backends (`--normal-backend`):
  - `deepbump` : Runs `deepbump256.onnx` via `onnxruntime` (prefers
                 `CoreMLExecutionProvider` on Apple Silicon with transparent
                 fallback to `CPUExecutionProvider`). Uses smooth 2D raised-cosine
                 overlap tiling and periodic wrap-around padding (`wrap_x`/`wrap_y`)
                 so seamless road/terrain normal maps have zero seams.
  - `hybrid`   : Combines DeepBump256 neural surface relief (which ignores painted
                 markings/decals) with crisp high-frequency Scharr micro-detail
                 for roads/stone/props, while applying chroma-gradient suppression
                 on `car_livery` so painted decals never produce false bumps.
  - `scharr`   : Fast zero-dependency multi-scale Scharr luminance normal map.
  - `auto`     : Selects `hybrid` when `onnxruntime` + `numpy` + `deepbump256.onnx`
                 are available, otherwise `scharr`.

PBR Specular / Roughness / Reflection / Clearcoat (`*_s`) channel layout
(100% compatible with OpenCMR2 Stage 6 + Stage 13 PBR):
  - R : Specular / Gloss (inverse roughness: high = glossy/smooth, low = matte)
  - G : Cubemap / Fresnel reflection & metallic boost (high = glass/chrome/paint)
  - B : Clearcoat (high = car clearcoat paint/glass/lights) OR micro-cavity AO
        accent for matte terrain/props (0 for foliage_alpha)
  - A : 255
"""

from __future__ import annotations

import math
import os
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple, Union

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from _image_io import HAS_NUMPY, RGBAImage
else:
    from ._image_io import HAS_NUMPY, RGBAImage

if HAS_NUMPY:
    import numpy as np  # type: ignore

try:
    import onnxruntime as ort  # type: ignore

    HAS_ORT = True
except ImportError:
    ort = None
    HAS_ORT = False

NORMAL_BACKENDS = ("auto", "deepbump", "hybrid", "scharr")

# Cache loaded ONNX sessions by resolved model path
_DEEPBUMP_CACHE: Dict[str, "DeepBumpSession"] = {}


# ---------------------------------------------------------------------------
# Discovery & backend resolution
# ---------------------------------------------------------------------------


def find_deepbump_model(explicit_path: Optional[Union[str, Path]] = None) -> Optional[Path]:
    """Locate `deepbump256.onnx`."""
    candidates: List[Path] = []
    if explicit_path:
        candidates.append(Path(explicit_path).expanduser())
    env_model = os.environ.get("OPENCMR2_DEEPBUMP_MODEL")
    if env_model:
        candidates.append(Path(env_model).expanduser())

    candidates.append(Path.home() / ".cache" / "opencmr2_hd" / "models" / "deepbump256.onnx")
    candidates.append(Path(__file__).resolve().parent / "models" / "deepbump256.onnx")

    for cand in candidates:
        if cand.is_file():
            return cand
    return None


def resolve_normal_backend(
    requested: str = "auto",
    deepbump_model: Optional[Union[str, Path]] = None,
) -> str:
    """Resolve requested normal backend into a concrete backend name."""
    req = (requested or "auto").lower()
    if req not in NORMAL_BACKENDS:
        raise ValueError(f"Unknown normal backend '{requested}' (expected one of {NORMAL_BACKENDS})")
    if req != "auto":
        return req
    if HAS_ORT and HAS_NUMPY and find_deepbump_model(deepbump_model) is not None:
        return "hybrid"
    return "scharr"


def normal_backend_summary(
    requested: str = "auto",
    deepbump_model: Optional[Union[str, Path]] = None,
) -> str:
    """Return a concise description of the active normal-map backend."""
    resolved = resolve_normal_backend(requested, deepbump_model=deepbump_model)
    if resolved in ("deepbump", "hybrid"):
        model_path = find_deepbump_model(deepbump_model)
        provs = ort.get_available_providers() if HAS_ORT else []
        ep = "CoreML" if "CoreMLExecutionProvider" in provs else "CPU"
        mname = model_path.name if model_path is not None else "missing model"
        return f"{resolved} [DeepBump256 {ep}, {mname}]"
    return "scharr (multi-scale luminance)"


# ---------------------------------------------------------------------------
# DeepBump256 ONNX inference with seamless periodic wrap & smooth overlap blend
# ---------------------------------------------------------------------------


class DeepBumpSession:
    """Cached ONNX Runtime inference session for DeepBump256."""

    def __init__(self, model_path: Path):
        if not HAS_ORT or not HAS_NUMPY:
            raise RuntimeError("DeepBump backend requires onnxruntime and numpy")
        self.model_path = Path(model_path)
        if not self.model_path.is_file():
            raise FileNotFoundError(f"DeepBump ONNX model not found: {self.model_path}")

        sess_opts = ort.SessionOptions()
        sess_opts.log_severity_level = 3

        avail = ort.get_available_providers()
        preferred = (
            ["CoreMLExecutionProvider", "CPUExecutionProvider"]
            if "CoreMLExecutionProvider" in avail
            else ["CPUExecutionProvider"]
        )

        self.provider = "CPUExecutionProvider"
        try:
            self.session = ort.InferenceSession(str(self.model_path), sess_options=sess_opts, providers=preferred)
            active = self.session.get_providers()
            self.provider = active[0] if active else "CPUExecutionProvider"
        except Exception:
            self.session = ort.InferenceSession(
                str(self.model_path), sess_options=sess_opts, providers=["CPUExecutionProvider"]
            )
            self.provider = "CPUExecutionProvider"

        inp = self.session.get_inputs()[0]
        out = self.session.get_outputs()[0]
        self.input_name: str = inp.name
        self.output_name: str = out.name

        shape = inp.shape
        self.tile_h: int = int(shape[2]) if len(shape) >= 4 and isinstance(shape[2], int) and shape[2] > 0 else 256
        self.tile_w: int = int(shape[3]) if len(shape) >= 4 and isinstance(shape[3], int) and shape[3] > 0 else 256

    def _run_tile(self, tile_luma: "np.ndarray") -> "np.ndarray":
        """Run one (tile_h, tile_w) float32 [0,1] tile -> (3, tile_h, tile_w) float32 [0,1]."""
        inp = np.ascontiguousarray(tile_luma[None, None, :, :], dtype=np.float32)
        try:
            out = self.session.run([self.output_name], {self.input_name: inp})[0]
        except Exception:
            # Fallback to CPU EP if CoreML fails at runtime (e.g., restricted sandbox)
            if self.provider != "CPUExecutionProvider":
                sess_opts = ort.SessionOptions()
                sess_opts.log_severity_level = 3
                self.session = ort.InferenceSession(
                    str(self.model_path), sess_options=sess_opts, providers=["CPUExecutionProvider"]
                )
                self.provider = "CPUExecutionProvider"
                out = self.session.run([self.output_name], {self.input_name: inp})[0]
            else:
                raise
        return out[0].astype(np.float32)

    def predict_normals_xyz(
        self,
        luma: "np.ndarray",
        wrap_x: bool = False,
        wrap_y: bool = False,
        overlap: int = 64,
    ) -> Tuple["np.ndarray", "np.ndarray", "np.ndarray"]:
        """Predict OpenCMR2 tangent-space unit normal `(nx, ny, nz)` from `(H, W)` float32 luma.

        Uses cyclic modulo-accumulated raised-cosine tiling on periodic wrapped axes
        (`wrap_x`/`wrap_y`) so opposite borders sit inside the interior of the same
        256x256 tile with zero seam discontinuity. Flips DeepBump's OpenGL (+Y up)
        output into OpenCMR2 DirectX (+Y down) tangent space to align with `fixed.frag`.
        """
        h, w = luma.shape
        if float(luma.max() - luma.min()) < 1e-4:
            zeros = np.zeros((h, w), dtype=np.float32)
            ones = np.ones((h, w), dtype=np.float32)
            return zeros, zeros, ones

        th, tw = self.tile_h, self.tile_w
        ov_y = min(overlap, th // 4)
        ov_x = min(overlap, tw // 4)

        work = luma.astype(np.float32)

        def _axis_plan(length: int, tile_len: int, ov: int, wrap: bool):
            """Return (starts, indices_list, weights_list, out_len) for one axis."""
            ramp_up = 0.5 - 0.5 * np.cos(np.linspace(0.0, math.pi, ov, dtype=np.float32))
            ramp_dn = ramp_up[::-1]
            if wrap:
                # Cyclic modulo-accumulated tiles: seam 0 <-> length-1 falls strictly
                # inside the flat-weight interior of a wrapping tile.
                if length <= tile_len - 2 * ov:
                    n_tiles = 1
                    starts = [0]
                else:
                    n_tiles = max(2, math.ceil(length / float(tile_len - ov)))
                    starts = [int(round(k * length / float(n_tiles))) % length for k in range(n_tiles)]
                w_base = np.ones(tile_len, dtype=np.float32)
                w_base[:ov] *= ramp_up
                w_base[-ov:] *= ramp_dn
                idx_list = [(np.arange(s, s + tile_len) % length) for s in starts]
                w_list = [w_base for _ in starts]
                return starts, idx_list, w_list

            # Non-wrapping axis: reflect/edge pad only if smaller than tile_len
            stride = max(32, tile_len - ov)
            if length <= tile_len:
                starts = [0]
            else:
                starts = list(range(0, length - tile_len, stride))
                if not starts or starts[-1] != length - tile_len:
                    starts.append(length - tile_len)

            idx_list = []
            w_list = []
            eff_len = max(length, tile_len)
            for s in starts:
                e = s + tile_len
                w_vec = np.ones(tile_len, dtype=np.float32)
                if s > 0:
                    w_vec[:ov] *= np.maximum(ramp_up, 1e-3)
                if e < eff_len:
                    w_vec[-ov:] *= np.maximum(ramp_dn, 1e-3)
                idx_list.append(np.arange(s, e))
                w_list.append(w_vec)
            return starts, idx_list, w_list

        # Pad non-wrapping axes up to tile_len if smaller than model tile size
        pad_b = 0 if wrap_y else max(0, th - h)
        pad_r = 0 if wrap_x else max(0, tw - w)
        if pad_b > 0:
            work = np.pad(work, ((0, pad_b), (0, 0)), mode="reflect" if h > 1 else "edge")
        if pad_r > 0:
            work = np.pad(work, ((0, 0), (0, pad_r)), mode="reflect" if w > 1 else "edge")

        acc_h = h if wrap_y else work.shape[0]
        acc_w = w if wrap_x else work.shape[1]

        _, y_idxs, y_ws = _axis_plan(h, th, ov_y, wrap_y)
        _, x_idxs, x_ws = _axis_plan(w, tw, ov_x, wrap_x)

        acc = np.zeros((acc_h, acc_w, 3), dtype=np.float32)
        w_acc = np.zeros((acc_h, acc_w), dtype=np.float32)

        for yi, wy in zip(y_idxs, y_ws):
            rows = work[yi, :]
            for xi, wx in zip(x_idxs, x_ws):
                tile = rows[:, xi]
                pred = self._run_tile(tile)  # (3, th, tw)
                w2d = wy[:, None] * wx[None, :]
                weighted = (pred * w2d[None, :, :]).transpose(1, 2, 0)  # (th, tw, 3)
                if wrap_y or wrap_x:
                    # np.add.at handles duplicate modulo indices cleanly (e.g., when h < th)
                    yy, xx = np.meshgrid(yi, xi, indexing="ij")
                    np.add.at(acc, (yy, xx), weighted)
                    np.add.at(w_acc, (yy, xx), w2d)
                else:
                    y0, x0 = int(yi[0]), int(xi[0])
                    acc[y0 : y0 + th, x0 : x0 + tw] += weighted
                    w_acc[y0 : y0 + th, x0 : x0 + tw] += w2d

        norm_rgb = acc / np.maximum(w_acc[:, :, None], 1e-6)
        cropped = norm_rgb[:h, :w, :]

        # Convert [0, 1] OpenGL (+Y up) -> DirectX (+Y down) unit tangent normal
        nx = cropped[:, :, 0] * 2.0 - 1.0
        ny = -(cropped[:, :, 1] * 2.0 - 1.0)
        nz = np.maximum(cropped[:, :, 2] * 2.0 - 1.0, 0.05)

        inv_len = 1.0 / np.sqrt(nx * nx + ny * ny + nz * nz)
        return nx * inv_len, ny * inv_len, nz * inv_len


def get_deepbump_session(deepbump_model: Optional[Union[str, Path]] = None) -> DeepBumpSession:
    """Return a cached DeepBumpSession for the discovered ONNX model."""
    model_path = find_deepbump_model(deepbump_model)
    if model_path is None:
        raise FileNotFoundError(
            "DeepBump256 model not found. Expected ~/.cache/opencmr2_hd/models/deepbump256.onnx or --deepbump-model PATH"
        )
    key = str(model_path.resolve())
    sess = _DEEPBUMP_CACHE.get(key)
    if sess is None:
        sess = DeepBumpSession(model_path)
        _DEEPBUMP_CACHE[key] = sess
    return sess


# ---------------------------------------------------------------------------
# Normal map helpers (Scharr + Chroma suppression + Hybrid DeepBump blending)
# ---------------------------------------------------------------------------

# (deepbump_strength, scharr_hybrid_weight, chroma_suppress)
_NORMAL_TUNING: Dict[str, Tuple[float, float, bool]] = {
    "road_surface": (1.15, 0.48, False),
    "road_tarmac": (1.10, 0.45, False),
    "road_gravel": (1.35, 0.62, False),
    "road_snow": (0.95, 0.28, False),
    "buildings_props": (1.25, 0.42, False),
    "foliage_alpha": (0.95, 0.35, False),
    "car_livery": (0.45, 0.10, True),
    "car_body": (0.45, 0.10, True),
    "car_glass": (0.18, 0.04, True),
    "car_cockpit": (0.78, 0.28, False),
    "car_wheel": (1.05, 0.42, False),
    "car_light": (0.65, 0.24, False),
}


def _pad_2d_numpy(field: "np.ndarray", wrap_x: bool, wrap_y: bool) -> "np.ndarray":
    p = np.pad(field, ((1, 1), (0, 0)), mode="wrap" if wrap_y else "edge")
    return np.pad(p, ((0, 0), (1, 1)), mode="wrap" if wrap_x else "edge")


def _scharr_2d_numpy(field: "np.ndarray", wrap_x: bool, wrap_y: bool) -> Tuple["np.ndarray", "np.ndarray"]:
    p = _pad_2d_numpy(field, wrap_x=wrap_x, wrap_y=wrap_y)
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


def _chroma_keep_mask_numpy(
    r: "np.ndarray",
    g: "np.ndarray",
    b: "np.ndarray",
    luma: "np.ndarray",
    wrap_x: bool,
    wrap_y: bool,
) -> "np.ndarray":
    """Return [0.12..1.0] mask attenuating normals across sharp paint/decal chroma edges."""
    cr = r - luma
    cb = b - luma
    gxr, gyr = _scharr_2d_numpy(cr, wrap_x=wrap_x, wrap_y=wrap_y)
    gxb, gyb = _scharr_2d_numpy(cb, wrap_x=wrap_x, wrap_y=wrap_y)
    chroma_mag = np.sqrt(gxr * gxr + gyr * gyr + gxb * gxb + gyb * gyb)
    return 0.12 + 0.88 / (1.0 + (chroma_mag * 14.0) ** 2)


def generate_neural_normal_map(
    image: RGBAImage,
    category: str = "buildings_props",
    sub_profile: str = "",
    wrap_x: bool = False,
    wrap_y: bool = False,
    backend: str = "auto",
    deepbump_model: Optional[Union[str, Path]] = None,
    strength_override: Optional[float] = None,
) -> Tuple[RGBAImage, str]:
    """Generate a tangent-space normal map using `deepbump`, `hybrid`, or `scharr`.

    Returns `(normal_image, resolved_backend_name)`.
    Neutral flat normal is encoded as `(128, 128, 255, 255)`.
    """
    resolved = resolve_normal_backend(backend, deepbump_model=deepbump_model)
    if resolved == "scharr" or not HAS_NUMPY:
        if __package__ in (None, ""):
            from upscale_hd import generate_normal_map as scharr_normal
        else:
            from .upscale_hd import generate_normal_map as scharr_normal
        return (
            scharr_normal(
                image,
                category=category,
                wrap_x=wrap_x,
                wrap_y=wrap_y,
                strength_override=strength_override,
            ),
            "scharr",
        )

    arr = np.frombuffer(image.data, dtype=np.uint8).reshape((image.height, image.width, 4)).astype(np.float32)
    r = arr[:, :, 0] / 255.0
    g = arr[:, :, 1] / 255.0
    b = arr[:, :, 2] / 255.0
    a = arr[:, :, 3] / 255.0
    luma = 0.299 * r + 0.587 * g + 0.114 * b

    tuning_key = sub_profile if sub_profile in _NORMAL_TUNING else category
    db_strength, sc_weight, use_chroma_suppress = _NORMAL_TUNING.get(tuning_key, (1.15, 0.40, False))
    if strength_override is not None:
        scale_ratio = float(strength_override) / max(0.05, db_strength)
        db_strength = float(strength_override)
        sc_weight *= min(2.0, max(0.0, scale_ratio))

    sess = get_deepbump_session(deepbump_model=deepbump_model)
    max_db_dim = 1024
    lh, lw = luma.shape
    if max(lh, lw) > max_db_dim:
        # Run DeepBump256 on a <=1024px view so its 256px receptive field captures
        # true macro surface relief (4-16x faster on 2K/4K), then upsample smoothly
        # and combine with full-resolution Scharr micro-detail below.
        ratio = max_db_dim / float(max(lh, lw))
        dw = max(16, int(round(lw * ratio)))
        dh = max(16, int(round(lh * ratio)))
        from PIL import Image as PILImage  # type: ignore

        luma_small = np.asarray(
            PILImage.fromarray(luma, mode="F").resize((dw, dh), resample=PILImage.Resampling.BILINEAR),
            dtype=np.float32,
        )
        nx_s, ny_s, nz_s = sess.predict_normals_xyz(luma_small, wrap_x=wrap_x, wrap_y=wrap_y)
        nx_db = np.asarray(
            PILImage.fromarray(nx_s, mode="F").resize((lw, lh), resample=PILImage.Resampling.BILINEAR),
            dtype=np.float32,
        )
        ny_db = np.asarray(
            PILImage.fromarray(ny_s, mode="F").resize((lw, lh), resample=PILImage.Resampling.BILINEAR),
            dtype=np.float32,
        )
        nz_db = np.asarray(
            PILImage.fromarray(nz_s, mode="F").resize((lw, lh), resample=PILImage.Resampling.BILINEAR),
            dtype=np.float32,
        )
        inv_l = 1.0 / np.sqrt(nx_db * nx_db + ny_db * ny_db + nz_db * nz_db + 1e-8)
        nx_db *= inv_l
        ny_db *= inv_l
        nz_db = np.maximum(nz_db * inv_l, 0.05)
    else:
        nx_db, ny_db, nz_db = sess.predict_normals_xyz(luma, wrap_x=wrap_x, wrap_y=wrap_y)

    # Convert unit DeepBump normal to tangent slopes
    inv_z = 1.0 / np.maximum(nz_db, 0.15)
    sx = nx_db * inv_z * db_strength
    sy = ny_db * inv_z * db_strength

    if resolved == "hybrid" and sc_weight > 0.0:
        # High-frequency micro-detail Scharr gradient (luma minus 3x3 Gaussian blur)
        p_l = _pad_2d_numpy(luma, wrap_x=wrap_x, wrap_y=wrap_y)
        blur = (
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
        # Blend fine + high-frequency Scharr detail into DeepBump relief
        gx_f, gy_f = _scharr_2d_numpy(luma, wrap_x=wrap_x, wrap_y=wrap_y)
        gx_hf, gy_hf = _scharr_2d_numpy(luma - blur, wrap_x=wrap_x, wrap_y=wrap_y)
        gx_detail = 0.55 * gx_f + 0.45 * gx_hf
        gy_detail = 0.55 * gy_f + 0.45 * gy_hf
        sx = sx + (-gx_detail) * (sc_weight * 2.4)
        sy = sy + (-gy_detail) * (sc_weight * 2.4)

    if use_chroma_suppress:
        keep = _chroma_keep_mask_numpy(r, g, b, luma, wrap_x=wrap_x, wrap_y=wrap_y)
        sx *= keep
        sy *= keep

    if category == "foliage_alpha":
        mask = np.clip((a - 0.1) / 0.4, 0.0, 1.0)
        sx *= mask
        sy *= mask

    inv_len = 1.0 / np.sqrt(sx * sx + sy * sy + 1.0)
    nx = sx * inv_len
    ny = sy * inv_len
    nz = inv_len

    out = np.empty((image.height, image.width, 4), dtype=np.uint8)
    out[:, :, 0] = np.clip(np.floor((nx * 0.5 + 0.5) * 255.0 + 0.5), 0, 255).astype(np.uint8)
    out[:, :, 1] = np.clip(np.floor((ny * 0.5 + 0.5) * 255.0 + 0.5), 0, 255).astype(np.uint8)
    out[:, :, 2] = np.clip(np.floor((nz * 0.5 + 0.5) * 255.0 + 0.5), 0, 255).astype(np.uint8)
    out[:, :, 3] = 255
    return RGBAImage(image.width, image.height, bytearray(out.tobytes())), resolved


# ---------------------------------------------------------------------------
# Stage 10 PBR Roughness / Specular / Reflection / Clearcoat map generation
# ---------------------------------------------------------------------------


def _smoothstep_scalar(edge0: float, edge1: float, x: float) -> float:
    if edge0 == edge1:
        return 1.0 if x >= edge1 else 0.0
    t = (x - edge0) / (edge1 - edge0)
    if t <= 0.0:
        return 0.0
    if t >= 1.0:
        return 1.0
    return t * t * (3.0 - 2.0 * t)


def _smoothstep_np(edge0: float, edge1: float, x: "np.ndarray") -> "np.ndarray":
    t = np.clip((x - edge0) / (edge1 - edge0), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def _clamp_byte(v: float) -> int:
    iv = int(v + 0.5) if v >= 0.0 else 0
    if iv < 0:
        return 0
    if iv > 255:
        return 255
    return iv


def _local_stats_numpy(
    luma: "np.ndarray",
    wrap_x: bool,
    wrap_y: bool,
) -> Tuple["np.ndarray", "np.ndarray"]:
    """Compute 3x3 local mean and local std-dev of luminance."""
    p = _pad_2d_numpy(luma, wrap_x=wrap_x, wrap_y=wrap_y)
    mean = (
        p[:-2, :-2]
        + p[:-2, 1:-1]
        + p[:-2, 2:]
        + p[1:-1, :-2]
        + p[1:-1, 1:-1]
        + p[1:-1, 2:]
        + p[2:, :-2]
        + p[2:, 1:-1]
        + p[2:, 2:]
    ) / 9.0
    p2 = p * p
    mean2 = (
        p2[:-2, :-2]
        + p2[:-2, 1:-1]
        + p2[:-2, 2:]
        + p2[1:-1, :-2]
        + p2[1:-1, 1:-1]
        + p2[1:-1, 2:]
        + p2[2:, :-2]
        + p2[2:, 1:-1]
        + p2[2:, 2:]
    ) / 9.0
    std = np.sqrt(np.maximum(mean2 - mean * mean, 0.0))
    return mean, std


def generate_pbr_specular_map(
    image: RGBAImage,
    category: str = "buildings_props",
    sub_profile: str = "",
    wrap_x: bool = False,
    wrap_y: bool = False,
) -> RGBAImage:
    """Generate a Stage 10 PBR specular/roughness/reflection/clearcoat map (`*_s`).

    Channel semantics (100% compatible with OpenCMR2 Stage 6 + Stage 13 PBR):
      - R : Specular / Gloss (inverse roughness: high = glossy, low = matte)
      - G : Cubemap / Fresnel reflection & metallic boost
      - B : Clearcoat (car paint/glass/lights) OR micro-cavity AO accent (roads/props; 0 for foliage)
      - A : 255

    Supports dedicated pixel-level material segmentation for `car_body`,
    `car_cockpit`, `car_glass`, `car_wheel`, `car_light`, `road_tarmac`,
    `road_gravel`, and `road_snow`.
    """
    w, h = image.width, image.height
    eff_sub = sub_profile
    if not eff_sub:
        if category == "car_livery":
            eff_sub = "car_body"
        elif category == "road_surface":
            eff_sub = "road_tarmac"

    if HAS_NUMPY:
        arr = np.frombuffer(image.data, dtype=np.uint8).reshape((h, w, 4)).astype(np.float32)
        r = arr[:, :, 0] / 255.0
        g = arr[:, :, 1] / 255.0
        b = arr[:, :, 2] / 255.0
        a = arr[:, :, 3] / 255.0

        luma = 0.299 * r + 0.587 * g + 0.114 * b
        cmax = np.maximum(np.maximum(r, g), b)
        cmin = np.minimum(np.minimum(r, g), b)
        sat = np.where(cmax > 1e-4, (cmax - cmin) / np.maximum(cmax, 1e-4), 0.0)
        local_mean, local_std = _local_stats_numpy(luma, wrap_x=wrap_x, wrap_y=wrap_y)
        cavity = np.clip((local_mean - luma) * 3.5, 0.0, 1.0)

        if category == "car_livery":
            if eff_sub == "car_cockpit":
                # Matte alcantara/dashboard/fabric vs glossy glass gauges & metallic trim
                tex_rough = _smoothstep_np(0.025, 0.09, local_std)
                bright_trim = _smoothstep_np(0.42, 0.72, luma) * (1.0 - 0.45 * sat)
                gloss_dial = bright_trim * (1.0 - 0.35 * tex_rough)
                base_spec = 32.0 + luma * 26.0 - cavity * 12.0
                base_refl = 16.0 + luma * 20.0 - cavity * 8.0
                base_coat = 8.0 + luma * 12.0 - cavity * 6.0
                spec = base_spec * (1.0 - gloss_dial) + (195.0 + luma * 42.0) * gloss_dial
                refl = base_refl * (1.0 - gloss_dial) + (182.0 + luma * 48.0) * gloss_dial
                coat = base_coat * (1.0 - gloss_dial) + (158.0 + luma * 54.0) * gloss_dial
            elif eff_sub == "car_glass":
                # High-gloss reflective windscreen & side windows; matte dark opaque frame trim
                frame_mask = _smoothstep_np(0.08, 0.03, luma) * _smoothstep_np(0.22, 0.08, sat)
                spec = (224.0 + luma * 26.0) * (1.0 - frame_mask) + 38.0 * frame_mask
                refl = (232.0 + (1.0 - sat) * 20.0) * (1.0 - frame_mask) + 22.0 * frame_mask
                coat = (218.0 + luma * 28.0) * (1.0 - frame_mask) + 14.0 * frame_mask
            elif eff_sub == "car_wheel":
                # Matte black rubber tyres vs bright/silver metallic alloy rims
                rim_mask = np.clip(
                    _smoothstep_np(0.30, 0.56, luma) * (1.0 - 0.25 * sat)
                    + _smoothstep_np(0.50, 0.72, luma) * 0.4,
                    0.0,
                    1.0,
                )
                tyre_spec = 30.0 + luma * 32.0 - cavity * 10.0
                tyre_refl = 14.0 + luma * 24.0 - cavity * 8.0
                tyre_coat = 6.0 + luma * 14.0 - cavity * 4.0
                rim_spec = 192.0 + luma * 46.0
                rim_refl = 188.0 + luma * 48.0
                rim_coat = 142.0 + luma * 52.0
                spec = tyre_spec * (1.0 - rim_mask) + rim_spec * rim_mask
                refl = tyre_refl * (1.0 - rim_mask) + rim_refl * rim_mask
                coat = tyre_coat * (1.0 - rim_mask) + rim_coat * rim_mask
            elif eff_sub == "car_light":
                # Glossy glass headlamp/taillight optics vs matte dark housing
                lens_mask = np.clip(_smoothstep_np(0.18, 0.42, luma) + _smoothstep_np(0.35, 0.65, sat) * 0.85, 0.0, 1.0)
                spec = 38.0 * (1.0 - lens_mask) + (214.0 + luma * 36.0) * lens_mask
                refl = 22.0 * (1.0 - lens_mask) + (206.0 + luma * 40.0) * lens_mask
                coat = 14.0 * (1.0 - lens_mask) + (208.0 + luma * 38.0) * lens_mask
            else:
                # car_body: pixel-level segmentation across:
                #   1. Painted livery panels -> high clearcoat B, strong gloss R & refl G
                #   2. Smooth dark glossy windows -> high refl G & gloss R & clearcoat B
                #   3. Matte tyres / rubber / grilles / vents / deep dark crevices -> LOW R, G, B
                tex_rough = _smoothstep_np(0.032, 0.105, local_std)
                achromatic = 1.0 - _smoothstep_np(0.18, 0.34, sat)
                deep_matte = _smoothstep_np(0.095, 0.035, luma) * achromatic
                textured_dark = _smoothstep_np(0.38, 0.18, luma) * achromatic * tex_rough
                matte_mask = np.clip(np.maximum(deep_matte, textured_dark), 0.0, 1.0)

                window_mask = (
                    _smoothstep_np(0.08, 0.14, luma)
                    * _smoothstep_np(0.36, 0.22, luma)
                    * achromatic
                    * (1.0 - tex_rough)
                    * (1.0 - matte_mask)
                )

                paint_spec = 182.0 + luma * 46.0 + sat * 22.0
                paint_refl = 176.0 + luma * 48.0 + sat * 20.0
                paint_coat = 202.0 + (0.45 * luma + 0.55 * sat) * 44.0

                win_spec = 228.0 + luma * 20.0
                win_refl = 238.0 + (1.0 - sat) * 14.0
                win_coat = 226.0 + luma * 18.0

                matte_spec = 32.0 + luma * 34.0 - cavity * 10.0
                matte_refl = 16.0 + luma * 24.0 - cavity * 8.0
                matte_coat = 8.0 + luma * 14.0 - cavity * 5.0

                spec = paint_spec * (1.0 - window_mask) + win_spec * window_mask
                refl = paint_refl * (1.0 - window_mask) + win_refl * window_mask
                coat = paint_coat * (1.0 - window_mask) + win_coat * window_mask

                spec = spec * (1.0 - matte_mask) + matte_spec * matte_mask
                refl = refl * (1.0 - matte_mask) + matte_refl * matte_mask
                coat = coat * (1.0 - matte_mask) + matte_coat * matte_mask

        elif category == "road_surface":
            ao = np.clip(1.0 - cavity * 0.65 - local_std * 0.8, 0.20, 1.0)
            if eff_sub == "road_snow":
                ice_boost = _smoothstep_np(0.55, 0.90, luma) * (1.0 - sat) * 72.0
                spec = 46.0 + (1.0 - sat * 0.5) * luma * 44.0 + ice_boost
                refl = 30.0 + (1.0 - sat * 0.6) * luma * 38.0 + ice_boost * 0.85
                coat = (18.0 + ice_boost * 0.85) * ao
            elif eff_sub == "road_gravel":
                spec = 24.0 + (1.0 - sat * 0.5) * luma * 30.0 - local_std * 24.0
                refl = 12.0 + (1.0 - sat * 0.6) * luma * 22.0 - local_std * 14.0
                coat = (26.0 * ao) + np.clip((luma - 0.72) / 0.28, 0.0, 1.0) * 18.0
            else:
                # road_tarmac (default road_surface)
                ice_boost = np.clip((luma - 0.68) / 0.32, 0.0, 1.0) * (1.0 - sat) * 48.0
                smooth_bonus = (1.0 - _smoothstep_np(0.03, 0.12, local_std)) * 12.0
                spec = 40.0 + (1.0 - sat * 0.5) * luma * 42.0 + smooth_bonus + ice_boost
                refl = 22.0 + (1.0 - sat * 0.6) * luma * 36.0 + smooth_bonus * 0.7 + ice_boost * 0.8
                coat = (30.0 * ao) + ice_boost * 0.55

        elif category == "foliage_alpha":
            mask = np.clip((a - 0.06) / 0.25, 0.0, 1.0)
            spec = (20.0 + luma * 24.0) * mask
            refl = (8.0 + luma * 16.0) * mask
            coat = np.zeros_like(luma)

        else:
            # buildings_props: matte walls + micro-cavity AO in B, slight sheen on bright glass/metal
            ao = np.clip(1.0 - cavity * 0.70 - local_std * 0.6, 0.18, 1.0)
            spec = 28.0 + luma * 42.0 - cavity * 14.0
            refl = 14.0 + luma * (1.0 - sat * 0.4) * 34.0 - cavity * 8.0
            coat = (24.0 * ao) + np.clip((luma - 0.6) * 2.5, 0.0, 1.0) * (1.0 - sat * 0.5) * 22.0

        out = np.empty((h, w, 4), dtype=np.uint8)
        out[:, :, 0] = np.clip(np.rint(spec), 0, 255).astype(np.uint8)
        out[:, :, 1] = np.clip(np.rint(refl), 0, 255).astype(np.uint8)
        out[:, :, 2] = np.clip(np.rint(coat), 0, 255).astype(np.uint8)
        out[:, :, 3] = 255
        return RGBAImage(w, h, bytearray(out.tobytes()))

    # Pure-Python fallback (identical material segmentation & local variance)
    src = image.data
    n = w * h
    inv255 = 1.0 / 255.0

    luma_arr = [0.0] * n
    sat_arr = [0.0] * n
    a_arr = [0.0] * n
    for i in range(n):
        p = i * 4
        r = src[p] * inv255
        g = src[p + 1] * inv255
        b = src[p + 2] * inv255
        a_arr[i] = src[p + 3] * inv255
        luma_arr[i] = 0.299 * r + 0.587 * g + 0.114 * b
        cmax = r if r >= g else g
        if b > cmax:
            cmax = b
        cmin = r if r <= g else g
        if b < cmin:
            cmin = b
        sat_arr[i] = ((cmax - cmin) / cmax) if cmax > 1e-4 else 0.0

    xm1 = [(x - 1) % w if wrap_x else (x - 1 if x > 0 else 0) for x in range(w)]
    xp1 = [(x + 1) % w if wrap_x else (x + 1 if x + 1 < w else w - 1) for x in range(w)]
    ym1 = [((y - 1) % h if wrap_y else (y - 1 if y > 0 else 0)) * w for y in range(h)]
    yp1 = [((y + 1) % h if wrap_y else (y + 1 if y + 1 < h else h - 1)) * w for y in range(h)]

    out = bytearray(n * 4)
    inv9 = 1.0 / 9.0

    for y in range(h):
        ru = ym1[y]
        rc = y * w
        rd = yp1[y]
        for x in range(w):
            xl = xm1[x]
            xr = xp1[x]
            idx = rc + x
            v0 = luma_arr[ru + xl]
            v1 = luma_arr[ru + x]
            v2 = luma_arr[ru + xr]
            v3 = luma_arr[rc + xl]
            v4 = luma_arr[idx]
            v5 = luma_arr[rc + xr]
            v6 = luma_arr[rd + xl]
            v7 = luma_arr[rd + x]
            v8 = luma_arr[rd + xr]

            lmean = (v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8) * inv9
            lmean2 = (
                v0 * v0
                + v1 * v1
                + v2 * v2
                + v3 * v3
                + v4 * v4
                + v5 * v5
                + v6 * v6
                + v7 * v7
                + v8 * v8
            ) * inv9
            var = lmean2 - lmean * lmean
            lstd = math.sqrt(var) if var > 0.0 else 0.0
            cavity = min(1.0, max(0.0, (lmean - v4) * 3.5))

            luma = v4
            sat = sat_arr[idx]
            a = a_arr[idx]

            if category == "car_livery":
                if eff_sub == "car_cockpit":
                    tex_rough = _smoothstep_scalar(0.025, 0.09, lstd)
                    bright_trim = _smoothstep_scalar(0.42, 0.72, luma) * (1.0 - 0.45 * sat)
                    gloss_dial = bright_trim * (1.0 - 0.35 * tex_rough)
                    base_spec = 32.0 + luma * 26.0 - cavity * 12.0
                    base_refl = 16.0 + luma * 20.0 - cavity * 8.0
                    base_coat = 8.0 + luma * 12.0 - cavity * 6.0
                    spec = base_spec * (1.0 - gloss_dial) + (195.0 + luma * 42.0) * gloss_dial
                    refl = base_refl * (1.0 - gloss_dial) + (182.0 + luma * 48.0) * gloss_dial
                    coat = base_coat * (1.0 - gloss_dial) + (158.0 + luma * 54.0) * gloss_dial
                elif eff_sub == "car_glass":
                    frame_mask = _smoothstep_scalar(0.08, 0.03, luma) * _smoothstep_scalar(0.22, 0.08, sat)
                    spec = (224.0 + luma * 26.0) * (1.0 - frame_mask) + 38.0 * frame_mask
                    refl = (232.0 + (1.0 - sat) * 20.0) * (1.0 - frame_mask) + 22.0 * frame_mask
                    coat = (218.0 + luma * 28.0) * (1.0 - frame_mask) + 14.0 * frame_mask
                elif eff_sub == "car_wheel":
                    rim_mask = min(
                        1.0,
                        max(
                            0.0,
                            _smoothstep_scalar(0.30, 0.56, luma) * (1.0 - 0.25 * sat)
                            + _smoothstep_scalar(0.50, 0.72, luma) * 0.4,
                        ),
                    )
                    tyre_spec = 30.0 + luma * 32.0 - cavity * 10.0
                    tyre_refl = 14.0 + luma * 24.0 - cavity * 8.0
                    tyre_coat = 6.0 + luma * 14.0 - cavity * 4.0
                    spec = tyre_spec * (1.0 - rim_mask) + (192.0 + luma * 46.0) * rim_mask
                    refl = tyre_refl * (1.0 - rim_mask) + (188.0 + luma * 48.0) * rim_mask
                    coat = tyre_coat * (1.0 - rim_mask) + (142.0 + luma * 52.0) * rim_mask
                elif eff_sub == "car_light":
                    lens_mask = min(
                        1.0,
                        max(0.0, _smoothstep_scalar(0.18, 0.42, luma) + _smoothstep_scalar(0.35, 0.65, sat) * 0.85),
                    )
                    spec = 38.0 * (1.0 - lens_mask) + (214.0 + luma * 36.0) * lens_mask
                    refl = 22.0 * (1.0 - lens_mask) + (206.0 + luma * 40.0) * lens_mask
                    coat = 14.0 * (1.0 - lens_mask) + (208.0 + luma * 38.0) * lens_mask
                else:
                    tex_rough = _smoothstep_scalar(0.032, 0.105, lstd)
                    achromatic = 1.0 - _smoothstep_scalar(0.18, 0.34, sat)
                    deep_matte = _smoothstep_scalar(0.095, 0.035, luma) * achromatic
                    textured_dark = _smoothstep_scalar(0.38, 0.18, luma) * achromatic * tex_rough
                    matte_mask = min(1.0, max(0.0, max(deep_matte, textured_dark)))

                    window_mask = (
                        _smoothstep_scalar(0.08, 0.14, luma)
                        * _smoothstep_scalar(0.36, 0.22, luma)
                        * achromatic
                        * (1.0 - tex_rough)
                        * (1.0 - matte_mask)
                    )

                    paint_spec = 182.0 + luma * 46.0 + sat * 22.0
                    paint_refl = 176.0 + luma * 48.0 + sat * 20.0
                    paint_coat = 202.0 + (0.45 * luma + 0.55 * sat) * 44.0

                    win_spec = 228.0 + luma * 20.0
                    win_refl = 238.0 + (1.0 - sat) * 14.0
                    win_coat = 226.0 + luma * 18.0

                    matte_spec = 32.0 + luma * 34.0 - cavity * 10.0
                    matte_refl = 16.0 + luma * 24.0 - cavity * 8.0
                    matte_coat = 8.0 + luma * 14.0 - cavity * 5.0

                    spec = paint_spec * (1.0 - window_mask) + win_spec * window_mask
                    refl = paint_refl * (1.0 - window_mask) + win_refl * window_mask
                    coat = paint_coat * (1.0 - window_mask) + win_coat * window_mask

                    spec = spec * (1.0 - matte_mask) + matte_spec * matte_mask
                    refl = refl * (1.0 - matte_mask) + matte_refl * matte_mask
                    coat = coat * (1.0 - matte_mask) + matte_coat * matte_mask

            elif category == "road_surface":
                ao = min(1.0, max(0.20, 1.0 - cavity * 0.65 - lstd * 0.8))
                if eff_sub == "road_snow":
                    ice_boost = _smoothstep_scalar(0.55, 0.90, luma) * (1.0 - sat) * 72.0
                    spec = 46.0 + (1.0 - sat * 0.5) * luma * 44.0 + ice_boost
                    refl = 30.0 + (1.0 - sat * 0.6) * luma * 38.0 + ice_boost * 0.85
                    coat = (18.0 + ice_boost * 0.85) * ao
                elif eff_sub == "road_gravel":
                    spec = 24.0 + (1.0 - sat * 0.5) * luma * 30.0 - lstd * 24.0
                    refl = 12.0 + (1.0 - sat * 0.6) * luma * 22.0 - lstd * 14.0
                    coat = (26.0 * ao) + max(0.0, min(1.0, (luma - 0.72) / 0.28)) * 18.0
                else:
                    ice_boost = max(0.0, min(1.0, (luma - 0.68) / 0.32)) * (1.0 - sat) * 48.0
                    smooth_bonus = (1.0 - _smoothstep_scalar(0.03, 0.12, lstd)) * 12.0
                    spec = 40.0 + (1.0 - sat * 0.5) * luma * 42.0 + smooth_bonus + ice_boost
                    refl = 22.0 + (1.0 - sat * 0.6) * luma * 36.0 + smooth_bonus * 0.7 + ice_boost * 0.8
                    coat = (30.0 * ao) + ice_boost * 0.55

            elif category == "foliage_alpha":
                mask = min(1.0, max(0.0, (a - 0.06) / 0.25))
                spec = (20.0 + luma * 24.0) * mask
                refl = (8.0 + luma * 16.0) * mask
                coat = 0.0
            else:
                ao = min(1.0, max(0.18, 1.0 - cavity * 0.70 - lstd * 0.6))
                spec = 28.0 + luma * 42.0 - cavity * 14.0
                refl = 14.0 + luma * (1.0 - sat * 0.4) * 34.0 - cavity * 8.0
                coat = (24.0 * ao) + max(0.0, min(1.0, (luma - 0.6) * 2.5)) * (1.0 - sat * 0.5) * 22.0

            di = idx * 4
            out[di] = _clamp_byte(spec)
            out[di + 1] = _clamp_byte(refl)
            out[di + 2] = _clamp_byte(coat)
            out[di + 3] = 255

    return RGBAImage(w, h, out)

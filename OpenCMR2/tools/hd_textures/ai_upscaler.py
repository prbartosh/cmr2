#!/usr/bin/env python3
"""Apple Silicon GPU (MPS) AI Super-Resolution & multi-backend upscaler for OpenCMR2.

Supported `--upscaler` backends:
  - `mps_esrgan` : Pure PyTorch RRDBNet (ESRGAN 4x, 23-block RRDB) with NO dependency
                   on `basicsr` or `spandrel`. Loads both classic ESRGAN checkpoints
                   (`4x-UltraSharp.pth`: `model.0.*` / `model.1.sub.0.RDB1...`) and
                   BasicSR/RealESRGAN checkpoints (`conv_first` / `body.0.rdb1...`).
                   Runs on Apple Silicon `mps` (fp16) when available, falling back
                   to `cpu`, with tiled overlap inference so 2K/4K textures never OOM.
  - `ncnn`       : Optional CLI wrapper for `realesrgan-ncnn-vulkan` (in PATH or
                   `~/.cache/opencmr2_hd/`).
  - `lanczos`    : Fast zero-dependency Lanczos-3 upscaler.
  - `auto`       : Selects `mps_esrgan` if PyTorch + model exist, else `ncnn` if
                   binary exists, else `lanczos`.

All AI backends preserve:
  - Periodic wrap-around padding for seamless road/terrain tiles (`wrap_x`/`wrap_y`)
  - Alpha-cutout RGB color dilation + clean Lanczos-3 alpha channel upscaling
  - Supersampled 4x -> Lanczos-3 downsample when targeting 2x (e.g., 512->1024)
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple, Union

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from _image_io import HAS_NUMPY, RGBAImage, load_png, save_png
else:
    from ._image_io import HAS_NUMPY, RGBAImage, load_png, save_png

if HAS_NUMPY:
    import numpy as np  # type: ignore

try:
    import torch  # type: ignore

    HAS_TORCH = True
except ImportError:
    torch = None
    HAS_TORCH = False

UPSCALER_BACKENDS = ("auto", "mps_esrgan", "ncnn", "lanczos")

DEFAULT_MODEL_NAMES = (
    "4x-UltraSharp.pth",
    "RealESRGAN_x4plus.pth",
    "RealESRGAN_x4plus.pt",
)

# Cache loaded PyTorch RRDBNet runners by (model_path, device_str, use_half)
_ESRGAN_CACHE: Dict[Tuple[str, str, bool], "MPSESRGANUpscaler"] = {}


# ---------------------------------------------------------------------------
# Discovery helpers
# ---------------------------------------------------------------------------


def find_ai_model(explicit_path: Optional[Union[str, Path]] = None) -> Optional[Path]:
    """Locate an ESRGAN 4x `.pth` checkpoint file."""
    candidates: List[Path] = []
    if explicit_path:
        candidates.append(Path(explicit_path).expanduser())
    env_model = os.environ.get("OPENCMR2_AI_MODEL")
    if env_model:
        candidates.append(Path(env_model).expanduser())

    cache_models = Path.home() / ".cache" / "opencmr2_hd" / "models"
    here_models = Path(__file__).resolve().parent / "models"
    for name in DEFAULT_MODEL_NAMES:
        candidates.append(cache_models / name)
        candidates.append(here_models / name)

    for cand in candidates:
        if cand.is_file():
            return cand
    return None


def find_ncnn_binary() -> Optional[Path]:
    """Locate `realesrgan-ncnn-vulkan` in PATH or `~/.cache/opencmr2_hd/`."""
    found = shutil.which("realesrgan-ncnn-vulkan")
    if found:
        return Path(found)

    cache_root = Path.home() / ".cache" / "opencmr2_hd"
    for rel in (
        "realesrgan-ncnn-vulkan",
        "bin/realesrgan-ncnn-vulkan",
        "ncnn/realesrgan-ncnn-vulkan",
    ):
        cand = cache_root / rel
        if cand.is_file() and os.access(cand, os.X_OK):
            return cand
    return None


def torch_mps_available() -> bool:
    """Return True if PyTorch with Apple Silicon MPS acceleration is available."""
    if not HAS_TORCH:
        return False
    try:
        return bool(torch.backends.mps.is_available())
    except Exception:
        return False


def resolve_upscaler_backend(
    requested: str = "auto",
    ai_model: Optional[Union[str, Path]] = None,
) -> str:
    """Resolve requested upscaler backend into a concrete backend name."""
    req = (requested or "auto").lower()
    if req not in UPSCALER_BACKENDS:
        raise ValueError(f"Unknown upscaler '{requested}' (expected one of {UPSCALER_BACKENDS})")
    if req != "auto":
        return req
    if HAS_TORCH and HAS_NUMPY and find_ai_model(ai_model) is not None:
        return "mps_esrgan"
    if find_ncnn_binary() is not None:
        return "ncnn"
    return "lanczos"


def upscaler_backend_summary(
    requested: str = "auto",
    ai_model: Optional[Union[str, Path]] = None,
) -> str:
    """Human-readable summary of the resolved upscaling backend."""
    resolved = resolve_upscaler_backend(requested, ai_model=ai_model)
    if resolved == "mps_esrgan":
        model_path = find_ai_model(ai_model)
        dev = "mps (fp16)" if torch_mps_available() else "cpu (fp32)"
        model_name = model_path.name if model_path is not None else "missing model"
        return f"mps_esrgan [{dev}, {model_name}]"
    if resolved == "ncnn":
        bin_path = find_ncnn_binary()
        return f"ncnn [{bin_path.name if bin_path else 'realesrgan-ncnn-vulkan'}]"
    return "lanczos (Faithful+ Lanczos-3)"


# ---------------------------------------------------------------------------
# ESRGAN / BasicSR state-dict converter (works even without PyTorch installed)
# ---------------------------------------------------------------------------

_OLD_RDB_RE = re.compile(r"^model\.1\.sub\.(\d+)\.RDB([123])\.conv([1-5])\.0\.(weight|bias)$")
_OLD_BODY_TAIL_RE = re.compile(r"^model\.1\.sub\.(\d+)\.(weight|bias)$")
_TRUNK_RDB_RE = re.compile(r"^RRDB_trunk\.(\d+)\.RDB([123])\.conv([1-5])\.(weight|bias)$")
_BASICSR_RDB_RE = re.compile(r"^body\.(\d+)\.rdb([123])\.conv([1-5])\.(weight|bias)$")


def _tensor_shape(tensor: Any) -> Optional[Tuple[int, ...]]:
    shape = getattr(tensor, "shape", None)
    if shape is None:
        return None
    try:
        return tuple(int(d) for d in shape)
    except Exception:
        return None


def convert_esrgan_state_dict(raw_state_dict: Dict[str, Any]) -> Tuple[Dict[str, Any], Dict[str, int]]:
    """Normalize an ESRGAN / Real-ESRGAN checkpoint into canonical RRDBNet keys.

    Supports:
      1. Classic ESRGAN keys (`4x-UltraSharp.pth`):
         `model.0.*`, `model.1.sub.{i}.RDB{1..3}.conv{1..5}.0.*`, `model.1.sub.{N}.*`,
         `model.3.*`, `model.6.*`, `model.8.*`, `model.10.*`
      2. BasicSR / Real-ESRGAN keys (`RealESRGAN_x4plus.pth`):
         `conv_first.*`, `body.{i}.rdb{1..3}.conv{1..5}.*`, `conv_body.*`,
         `conv_up1.*`, `conv_up2.*`, `conv_hr.*`, `conv_last.*`
      3. Classic RRDB_trunk keys:
         `conv_first.*`, `RRDB_trunk.{i}.RDB{1..3}.conv{1..5}.*`, `trunk_conv.*`,
         `upconv1.*`, `upconv2.*`, `HRconv.*`, `conv_last.*`

    Returns `(canonical_state_dict, arch_meta)` where `arch_meta` contains
    inferred `num_in_ch`, `num_out_ch`, `num_feat`, `num_block`, `num_grow_ch`, `scale`.
    """
    sd: Dict[str, Any] = raw_state_dict
    for wrapper_key in ("params_ema", "params", "state_dict", "model"):
        if wrapper_key in sd and isinstance(sd[wrapper_key], dict):
            sd = sd[wrapper_key]
            break

    # Strip DistributedDataParallel 'module.' prefix if present
    cleaned: Dict[str, Any] = {}
    for k, v in sd.items():
        nk = k[7:] if k.startswith("module.") else k
        cleaned[nk] = v

    converted: Dict[str, Any] = {}
    if "model.0.weight" in cleaned:
        # Classic ESRGAN sequential key format (e.g. 4x-UltraSharp.pth)
        max_rdb_idx = -1
        for k in cleaned:
            m = _OLD_RDB_RE.match(k)
            if m:
                max_rdb_idx = max(max_rdb_idx, int(m.group(1)))
        body_tail_idx = max_rdb_idx + 1 if max_rdb_idx >= 0 else 23

        # Collect top-level model.{idx}.weight indices after model.1 (upsampling & HR layers)
        post_indices = sorted(
            {
                int(k.split(".")[1])
                for k in cleaned
                if k.startswith("model.") and k.endswith(".weight") and k.count(".") == 2 and int(k.split(".")[1]) > 1
            }
        )
        # Standard 4x layout has 4 convs after body: [3, 6, 8, 10] -> up1, up2, hr, last
        # Standard 2x layout has 3 convs after body: [3, 5, 7] -> up1, hr, last
        if len(post_indices) == 4:
            post_map = {
                post_indices[0]: "conv_up1",
                post_indices[1]: "conv_up2",
                post_indices[2]: "conv_hr",
                post_indices[3]: "conv_last",
            }
        elif len(post_indices) == 3:
            post_map = {
                post_indices[0]: "conv_up1",
                post_indices[1]: "conv_hr",
                post_indices[2]: "conv_last",
            }
        else:
            post_map = {3: "conv_up1", 6: "conv_up2", 8: "conv_hr", 10: "conv_last"}

        for k, v in cleaned.items():
            if k in ("model.0.weight", "model.0.bias"):
                converted["conv_first." + k.rsplit(".", 1)[1]] = v
                continue
            m = _OLD_RDB_RE.match(k)
            if m:
                blk, rdb, conv, wb = m.groups()
                converted[f"body.{blk}.rdb{rdb}.conv{conv}.{wb}"] = v
                continue
            m_tail = _OLD_BODY_TAIL_RE.match(k)
            if m_tail and int(m_tail.group(1)) == body_tail_idx:
                converted[f"conv_body.{m_tail.group(2)}"] = v
                continue
            if k.startswith("model.") and k.count(".") == 2:
                _, idx_s, wb = k.split(".")
                idx = int(idx_s)
                if idx in post_map and wb in ("weight", "bias"):
                    converted[f"{post_map[idx]}.{wb}"] = v
                    continue
            converted[k] = v
    elif any(k.startswith("RRDB_trunk.") for k in cleaned):
        # Classic ESRGAN RRDB_trunk format
        simple_map = {
            "trunk_conv": "conv_body",
            "upconv1": "conv_up1",
            "upconv2": "conv_up2",
            "HRconv": "conv_hr",
        }
        for k, v in cleaned.items():
            m = _TRUNK_RDB_RE.match(k)
            if m:
                blk, rdb, conv, wb = m.groups()
                converted[f"body.{blk}.rdb{rdb}.conv{conv}.{wb}"] = v
                continue
            prefix, _, suffix = k.partition(".")
            if prefix in simple_map and suffix:
                converted[f"{simple_map[prefix]}.{suffix}"] = v
            else:
                converted[k] = v
    else:
        # BasicSR / Real-ESRGAN canonical format
        converted = dict(cleaned)

    # Infer architecture dimensions from canonical keys
    block_indices = set()
    for k in converted:
        m = _BASICSR_RDB_RE.match(k)
        if m:
            block_indices.add(int(m.group(1)))

    num_block = (max(block_indices) + 1) if block_indices else 23
    num_in_ch = 3
    num_out_ch = 3
    num_feat = 64
    num_grow_ch = 32

    s_first = _tensor_shape(converted.get("conv_first.weight"))
    if s_first and len(s_first) == 4:
        num_feat = s_first[0]
        num_in_ch = s_first[1]

    s_grow = _tensor_shape(converted.get("body.0.rdb1.conv1.weight"))
    if s_grow and len(s_grow) == 4:
        num_grow_ch = s_grow[0]

    s_last = _tensor_shape(converted.get("conv_last.weight"))
    if s_last and len(s_last) == 4:
        num_out_ch = s_last[0]

    scale = 4 if "conv_up2.weight" in converted else 2
    meta = {
        "num_in_ch": num_in_ch,
        "num_out_ch": num_out_ch,
        "num_feat": num_feat,
        "num_block": num_block,
        "num_grow_ch": num_grow_ch,
        "scale": scale,
    }
    return converted, meta


# ---------------------------------------------------------------------------
# Pure PyTorch RRDBNet implementation (no basicsr / spandrel required)
# ---------------------------------------------------------------------------


def _create_rrdbnet(
    num_in_ch: int = 3,
    num_out_ch: int = 3,
    num_feat: int = 64,
    num_block: int = 23,
    num_grow_ch: int = 32,
    scale: int = 4,
):
    """Instantiate a pure PyTorch RRDBNet matching ESRGAN / Real-ESRGAN."""
    if not HAS_TORCH:
        raise RuntimeError("PyTorch is not installed")

    import torch.nn as nn  # type: ignore
    import torch.nn.functional as F  # type: ignore

    class ResidualDenseBlock(nn.Module):
        def __init__(self, nf: int = 64, gc: int = 32):
            super().__init__()
            self.conv1 = nn.Conv2d(nf, gc, 3, 1, 1)
            self.conv2 = nn.Conv2d(nf + gc, gc, 3, 1, 1)
            self.conv3 = nn.Conv2d(nf + 2 * gc, gc, 3, 1, 1)
            self.conv4 = nn.Conv2d(nf + 3 * gc, gc, 3, 1, 1)
            self.conv5 = nn.Conv2d(nf + 4 * gc, nf, 3, 1, 1)
            self.lrelu = nn.LeakyReLU(negative_slope=0.2, inplace=True)

        def forward(self, x):
            x1 = self.lrelu(self.conv1(x))
            x2 = self.lrelu(self.conv2(torch.cat((x, x1), 1)))
            x3 = self.lrelu(self.conv3(torch.cat((x, x1, x2), 1)))
            x4 = self.lrelu(self.conv4(torch.cat((x, x1, x2, x3), 1)))
            x5 = self.conv5(torch.cat((x, x1, x2, x3, x4), 1))
            return x5 * 0.2 + x

    class RRDB(nn.Module):
        def __init__(self, nf: int, gc: int = 32):
            super().__init__()
            self.rdb1 = ResidualDenseBlock(nf, gc)
            self.rdb2 = ResidualDenseBlock(nf, gc)
            self.rdb3 = ResidualDenseBlock(nf, gc)

        def forward(self, x):
            out = self.rdb1(x)
            out = self.rdb2(out)
            out = self.rdb3(out)
            return out * 0.2 + x

    class RRDBNet(nn.Module):
        def __init__(self):
            super().__init__()
            self.scale = scale
            self.conv_first = nn.Conv2d(num_in_ch, num_feat, 3, 1, 1)
            self.body = nn.Sequential(*[RRDB(num_feat, num_grow_ch) for _ in range(num_block)])
            self.conv_body = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
            self.conv_up1 = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
            self.conv_up2 = nn.Conv2d(num_feat, num_feat, 3, 1, 1) if scale == 4 else None
            self.conv_hr = nn.Conv2d(num_feat, num_feat, 3, 1, 1)
            self.conv_last = nn.Conv2d(num_feat, num_out_ch, 3, 1, 1)
            self.lrelu = nn.LeakyReLU(negative_slope=0.2, inplace=True)

        def forward(self, x):
            feat = self.conv_first(x)
            body_feat = self.conv_body(self.body(feat))
            feat = feat + body_feat
            feat = self.lrelu(self.conv_up1(F.interpolate(feat, scale_factor=2, mode="nearest")))
            if self.conv_up2 is not None:
                feat = self.lrelu(self.conv_up2(F.interpolate(feat, scale_factor=2, mode="nearest")))
            out = self.conv_last(self.lrelu(self.conv_hr(feat)))
            return out

    return RRDBNet()


class MPSESRGANUpscaler:
    """Cached RRDBNet runner targeting Apple Silicon MPS (fp16) with CPU fallback."""

    def __init__(
        self,
        model_path: Path,
        device: Optional[str] = None,
        use_half: Optional[bool] = None,
    ):
        if not HAS_TORCH or not HAS_NUMPY:
            raise RuntimeError("mps_esrgan backend requires PyTorch and NumPy")

        self.model_path = Path(model_path)
        if not self.model_path.is_file():
            raise FileNotFoundError(f"ESRGAN model checkpoint not found: {self.model_path}")

        if device is None:
            device = "mps" if torch_mps_available() else "cpu"
        self.device_str = device
        self.device = torch.device(device)
        self.use_half = (device == "mps") if use_half is None else bool(use_half and device != "cpu")

        # Load checkpoint safely on CPU first, then convert keys
        try:
            raw_sd = torch.load(str(self.model_path), map_location="cpu", weights_only=True)
        except TypeError:
            raw_sd = torch.load(str(self.model_path), map_location="cpu")

        canonical_sd, meta = convert_esrgan_state_dict(raw_sd)
        self.scale = meta["scale"]
        self.arch_meta = meta

        model = _create_rrdbnet(
            num_in_ch=meta["num_in_ch"],
            num_out_ch=meta["num_out_ch"],
            num_feat=meta["num_feat"],
            num_block=meta["num_block"],
            num_grow_ch=meta["num_grow_ch"],
            scale=meta["scale"],
        )
        model.load_state_dict(canonical_sd, strict=True)
        model.eval()
        for p in model.parameters():
            p.requires_grad_(False)

        try:
            model = model.to(self.device)
            if self.use_half:
                model = model.half()
        except Exception:
            # Graceful fallback to CPU fp32 if MPS device transfer fails
            self.device_str = "cpu"
            self.device = torch.device("cpu")
            self.use_half = False
            model = model.float().to(self.device)

        self.model = model

    def _infer_tile(self, rgb_u8: "np.ndarray") -> "np.ndarray":
        """Run a single RGB tile (H, W, 3) uint8 -> (H*scale, W*scale, 3) float32 [0, 255]."""
        inp = np.ascontiguousarray(rgb_u8.transpose(2, 0, 1), dtype=np.float32) * (1.0 / 255.0)
        tensor = torch.from_numpy(inp).unsqueeze(0).to(self.device)
        if self.use_half:
            tensor = tensor.half()

        with torch.inference_mode():
            out = self.model(tensor)
            if self.use_half and not bool(torch.isfinite(out).all().item()):
                # Auto-fallback to fp32 if fp16 ever overflows on a specific checkpoint
                self.use_half = False
                self.model = self.model.float()
                out = self.model(tensor.float())
            out_np = out.squeeze(0).float().clamp(0.0, 1.0).cpu().numpy()
        return (out_np.transpose(1, 2, 0) * 255.0).astype(np.float32)

    def upscale_rgb(
        self,
        rgb_u8: "np.ndarray",
        tile_size: int = 256,
        tile_overlap: int = 16,
    ) -> "np.ndarray":
        """Upscale an (H, W, 3) uint8 RGB image by `self.scale` (typically 4x) with tiled blending."""
        h, w, _ = rgb_u8.shape
        s = self.scale

        if h <= tile_size and w <= tile_size:
            out_f = self._infer_tile(rgb_u8)
            return np.clip(np.rint(out_f), 0, 255).astype(np.uint8)

        tile_h = min(tile_size, h)
        tile_w = min(tile_size, w)
        overlap = min(tile_overlap, max(4, min(tile_h, tile_w) // 4))
        stride_y = max(16, tile_h - overlap)
        stride_x = max(16, tile_w - overlap)

        def _tile_starts(length: int, tsize: int, stride: int) -> List[int]:
            if length <= tsize:
                return [0]
            starts = list(range(0, length - tsize, stride))
            if not starts or starts[-1] != length - tsize:
                starts.append(length - tsize)
            return starts

        ys = _tile_starts(h, tile_h, stride_y)
        xs = _tile_starts(w, tile_w, stride_x)

        out_h, out_w = h * s, w * s
        acc = np.zeros((out_h, out_w, 3), dtype=np.float32)
        w_acc = np.zeros((out_h, out_w), dtype=np.float32)
        fade = max(4, overlap * s)

        for y0 in ys:
            y1 = min(h, y0 + tile_h)
            th_out = (y1 - y0) * s
            wy = np.ones(th_out, dtype=np.float32)
            if y0 > 0:
                f = min(fade, th_out)
                wy[:f] *= np.linspace(1e-3, 1.0, f, dtype=np.float32)
            if y1 < h:
                f = min(fade, th_out)
                wy[-f:] *= np.linspace(1.0, 1e-3, f, dtype=np.float32)

            for x0 in xs:
                x1 = min(w, x0 + tile_w)
                tw_out = (x1 - x0) * s
                wx = np.ones(tw_out, dtype=np.float32)
                if x0 > 0:
                    f = min(fade, tw_out)
                    wx[:f] *= np.linspace(1e-3, 1.0, f, dtype=np.float32)
                if x1 < w:
                    f = min(fade, tw_out)
                    wx[-f:] *= np.linspace(1.0, 1e-3, f, dtype=np.float32)

                tile_out = self._infer_tile(rgb_u8[y0:y1, x0:x1])
                w2d = wy[:, None] * wx[None, :]
                acc[y0 * s : y1 * s, x0 * s : x1 * s] += tile_out * w2d[:, :, None]
                w_acc[y0 * s : y1 * s, x0 * s : x1 * s] += w2d

        result = acc / np.maximum(w_acc[:, :, None], 1e-6)
        return np.clip(np.rint(result), 0, 255).astype(np.uint8)


def get_esrgan_upscaler(
    ai_model: Optional[Union[str, Path]] = None,
    device: Optional[str] = None,
    use_half: Optional[bool] = None,
) -> MPSESRGANUpscaler:
    """Return a cached MPSESRGANUpscaler instance for the given checkpoint."""
    model_path = find_ai_model(ai_model)
    if model_path is None:
        raise FileNotFoundError(
            "No ESRGAN checkpoint found. Expected ~/.cache/opencmr2_hd/models/4x-UltraSharp.pth or --ai-model PATH"
        )
    dev_key = device or ("mps" if torch_mps_available() else "cpu")
    half_key = (dev_key == "mps") if use_half is None else bool(use_half and dev_key != "cpu")
    cache_key = (str(model_path.resolve()), dev_key, half_key)
    runner = _ESRGAN_CACHE.get(cache_key)
    if runner is None:
        runner = MPSESRGANUpscaler(model_path, device=dev_key, use_half=half_key)
        _ESRGAN_CACHE[cache_key] = runner
    return runner


# ---------------------------------------------------------------------------
# Optional realesrgan-ncnn-vulkan CLI backend
# ---------------------------------------------------------------------------


def _run_ncnn_4x_rgb(image: RGBAImage) -> RGBAImage:
    """Invoke `realesrgan-ncnn-vulkan` on an opaque RGBAImage and return 4x RGBAImage."""
    bin_path = find_ncnn_binary()
    if bin_path is None:
        raise FileNotFoundError("realesrgan-ncnn-vulkan binary not found in PATH or ~/.cache/opencmr2_hd/")

    with tempfile.TemporaryDirectory(prefix="opencmr2_ncnn_") as td:
        td_path = Path(td)
        in_png = td_path / "in.png"
        out_png = td_path / "out.png"
        # Save with opaque alpha so NCNN focuses strictly on dilated RGB
        if HAS_NUMPY:
            arr = image.to_numpy()
            arr[:, :, 3] = 255
            opaque = RGBAImage.from_numpy(arr)
        else:
            buf = bytearray(image.data)
            for i in range(3, len(buf), 4):
                buf[i] = 255
            opaque = RGBAImage(image.width, image.height, buf)
        save_png(opaque, in_png, compress_level=1)

        cmd = [str(bin_path), "-i", str(in_png), "-o", str(out_png), "-s", "4"]
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        if proc.returncode != 0 or not out_png.is_file():
            raise RuntimeError(
                f"realesrgan-ncnn-vulkan failed (rc={proc.returncode}): {proc.stderr.decode('utf-8', 'ignore')}"
            )
        return load_png(out_png)


# ---------------------------------------------------------------------------
# Unified wrap-safe & alpha-halo-safe upscaling API
# ---------------------------------------------------------------------------


def _image_has_non_opaque_alpha(image: RGBAImage) -> bool:
    if HAS_NUMPY:
        arr = np.frombuffer(image.data, dtype=np.uint8).reshape((image.height, image.width, 4))
        return bool((arr[:, :, 3] < 255).any())
    d = image.data
    for i in range(3, len(d), 4):
        if d[i] < 255:
            return True
    return False


def _crop_rgba(image: RGBAImage, x0: int, y0: int, crop_w: int, crop_h: int) -> RGBAImage:
    if HAS_NUMPY:
        arr = np.frombuffer(image.data, dtype=np.uint8).reshape((image.height, image.width, 4))
        sub = np.ascontiguousarray(arr[y0 : y0 + crop_h, x0 : x0 + crop_w, :])
        return RGBAImage(crop_w, crop_h, bytearray(sub.tobytes()))
    src = image.data
    sw = image.width
    out = bytearray(crop_w * crop_h * 4)
    row_bytes = crop_w * 4
    for y in range(crop_h):
        si = ((y0 + y) * sw + x0) * 4
        di = y * row_bytes
        out[di : di + row_bytes] = src[si : si + row_bytes]
    return RGBAImage(crop_w, crop_h, out)


def _merge_rgb_and_alpha(rgb_img: RGBAImage, alpha_img: RGBAImage) -> RGBAImage:
    if rgb_img.width != alpha_img.width or rgb_img.height != alpha_img.height:
        raise ValueError("RGB and alpha dimensions must match")
    if HAS_NUMPY:
        rgb_arr = rgb_img.to_numpy()
        a_arr = np.frombuffer(alpha_img.data, dtype=np.uint8).reshape((alpha_img.height, alpha_img.width, 4))
        rgb_arr[:, :, 3] = a_arr[:, :, 3]
        return RGBAImage.from_numpy(rgb_arr)
    out = bytearray(rgb_img.data)
    src_a = alpha_img.data
    for i in range(3, len(out), 4):
        out[i] = src_a[i]
    return RGBAImage(rgb_img.width, rgb_img.height, out)


def upscale_image(
    image: RGBAImage,
    out_w: int,
    out_h: int,
    backend: str = "auto",
    wrap_x: bool = False,
    wrap_y: bool = False,
    has_alpha_cutout: bool = False,
    ai_model: Optional[Union[str, Path]] = None,
    tile_size: int = 256,
    tile_overlap: int = 16,
) -> Tuple[RGBAImage, str]:
    """Upscale an RGBAImage to `(out_w, out_h)` using the chosen backend.

    Returns `(upscaled_image, resolved_backend_name)`.
    Guarantees:
      - Periodic wrap-around padding before AI inference + exact center crop after
        when `wrap_x` or `wrap_y` is True.
      - Alpha-cutout RGB dilation before AI RGB inference + clean Lanczos-3 alpha
        upscaling so foliage/fences never exhibit dark/bright halos.
      - Supersampled 4x AI -> Lanczos-3 downsample when target resolution is 2x
        (or any size different from exact 4x).
    """
    if __package__ in (None, ""):
        from upscale_hd import dilate_alpha_rgb, lanczos_resize, pad_periodic
    else:
        from .upscale_hd import dilate_alpha_rgb, lanczos_resize, pad_periodic

    resolved = resolve_upscaler_backend(backend, ai_model=ai_model)
    src_w, src_h = image.width, image.height

    if src_w == out_w and src_h == out_h:
        return image.copy(), resolved

    # Always dilate transparent RGB before upscaling on alpha-cutout textures
    has_alpha = has_alpha_cutout or _image_has_non_opaque_alpha(image)
    work_src = dilate_alpha_rgb(image) if has_alpha else image

    if resolved == "lanczos":
        return lanczos_resize(work_src, out_w, out_h, wrap_x=wrap_x, wrap_y=wrap_y), "lanczos"

    # Upscale alpha channel cleanly with Lanczos-3 (zero AI hallucination in alpha)
    alpha_up = lanczos_resize(work_src, out_w, out_h, wrap_x=wrap_x, wrap_y=wrap_y) if has_alpha else None

    # Periodic wrap-around padding so AI convolution receptive fields wrap seamlessly
    pad_x = min(src_w, 16) if wrap_x else 0
    pad_y = min(src_h, 16) if wrap_y else 0
    padded = pad_periodic(work_src, pad_x, pad_y, wrap_x=wrap_x, wrap_y=wrap_y) if (pad_x or pad_y) else work_src

    if resolved == "mps_esrgan":
        runner = get_esrgan_upscaler(ai_model=ai_model)
        s = runner.scale
        rgb_in = np.frombuffer(padded.data, dtype=np.uint8).reshape((padded.height, padded.width, 4))[:, :, :3]
        rgb_out = runner.upscale_rgb(rgb_in, tile_size=tile_size, tile_overlap=tile_overlap)
        rgba_out = np.empty((rgb_out.shape[0], rgb_out.shape[1], 4), dtype=np.uint8)
        rgba_out[:, :, :3] = rgb_out
        rgba_out[:, :, 3] = 255
        ai_full = RGBAImage(rgba_out.shape[1], rgba_out.shape[0], bytearray(rgba_out.tobytes()))
    elif resolved == "ncnn":
        s = 4
        ai_full = _run_ncnn_4x_rgb(padded)
    else:
        return lanczos_resize(work_src, out_w, out_h, wrap_x=wrap_x, wrap_y=wrap_y), "lanczos"

    # Exact center crop to remove periodic wrap padding
    if pad_x or pad_y:
        ai_cropped = _crop_rgba(ai_full, pad_x * s, pad_y * s, src_w * s, src_h * s)
    else:
        ai_cropped = ai_full

    # Supersampled Lanczos-3 downsample when target is e.g. 2x (512->1024) instead of 4x
    if ai_cropped.width != out_w or ai_cropped.height != out_h:
        rgb_final = lanczos_resize(ai_cropped, out_w, out_h, wrap_x=wrap_x, wrap_y=wrap_y)
    else:
        rgb_final = ai_cropped

    if alpha_up is not None:
        rgb_final = _merge_rgb_and_alpha(rgb_final, alpha_up)

    return rgb_final, resolved

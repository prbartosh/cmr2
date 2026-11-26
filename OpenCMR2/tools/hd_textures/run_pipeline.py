#!/usr/bin/env python3
"""Stage 10 HD Texture Pipeline CLI for Colin McRae Rally 2.0 (OpenCMR2).

Orchestrates:
  1. Read-only scan of CMR2 `.BFL` archives and loose `.DDS`/`.TGA` textures
  2. Automatic texture classification + car/cockpit/road PBR sub-profile detection
     (with automatic skip of OpenCMR2 vectorized UI/font textures)
  3. HD upscaling (Apple Silicon MPS ESRGAN 4x, NCNN, or Faithful+ Lanczos-3)
     with periodic wrap tiling, alpha-cutout RGB color bleeding, hero 2K/4K
     resolution tiering, neural DeepBump256/Hybrid normal maps, and Stage 10
     PBR roughness/specular/reflection/clearcoat maps
  4. Native DX10 BC7 `.dds` (with full pre-generated mipmap chains) or `.png`
     output directly into an OpenCMR2 `overrides/` directory.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from collections import deque
from concurrent.futures import Future, ProcessPoolExecutor, ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Deque, Dict, Iterable, List, Optional, Sequence, Set, Tuple

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from _image_io import RGBAImage, backend_summary, save_png
    from ai_upscaler import (
        UPSCALER_BACKENDS,
        resolve_upscaler_backend,
        upscaler_backend_summary,
    )
    from bc7_dds import BC7_QUALITY_CHOICES, bc7_backend_summary
    from bfl_extract import TextureSource, load_texture_source, scan_data_directory
    from classify_textures import CATEGORIES, ClassificationResult, classify_texture
    from neural_maps import (
        NORMAL_BACKENDS,
        normal_backend_summary,
        resolve_normal_backend,
    )
    from upscale_hd import (
        CATEGORY_PROFILES,
        compute_effective_max_size,
        compute_target_size,
        is_hero_asset,
        process_texture,
        save_upscale_result,
    )
else:
    from ._image_io import RGBAImage, backend_summary, save_png
    from .ai_upscaler import (
        UPSCALER_BACKENDS,
        resolve_upscaler_backend,
        upscaler_backend_summary,
    )
    from .bc7_dds import BC7_QUALITY_CHOICES, bc7_backend_summary
    from .bfl_extract import TextureSource, load_texture_source, scan_data_directory
    from .classify_textures import CATEGORIES, ClassificationResult, classify_texture
    from .neural_maps import (
        NORMAL_BACKENDS,
        normal_backend_summary,
        resolve_normal_backend,
    )
    from .upscale_hd import (
        CATEGORY_PROFILES,
        compute_effective_max_size,
        compute_target_size,
        is_hero_asset,
        process_texture,
        save_upscale_result,
    )

OUTPUT_FORMAT_CHOICES = ("png", "dds", "bc7")


@dataclass(frozen=True)
class _WorkerJob:
    source: TextureSource
    output_dir: str
    allowed_categories: Optional[Tuple[str, ...]]
    scale: Optional[int]
    max_size: int
    hero_max_size: Optional[int]
    dry_run: bool
    overwrite: bool
    originals_dir: Optional[str]
    output_format: str = "png"
    upscaler: str = "lanczos"
    normal_backend: str = "scharr"
    ai_model: Optional[str] = None
    deepbump_model: Optional[str] = None
    bc7_quality: str = "fast"
    bc7_backend: str = "auto"
    bc7_threads: int = 0


def _output_ext(output_format: str) -> str:
    return "dds" if output_format.lower() in ("dds", "bc7") else "png"


def _decode_and_classify(
    job: _WorkerJob,
) -> Tuple[Optional[RGBAImage], Optional[ClassificationResult], Optional[Dict[str, object]], float]:
    """Decode & classify a single texture source on CPU.

    Returns `(img, cls, early_record, t0)`. When `early_record` is not None,
    the texture needs no further GPU/save work (skipped/filtered/error/dry_run/exists).
    """
    src = job.source
    t0 = time.monotonic()

    # Fast pre-check for exact UI/font skips before decoding pixels
    pre_cls: ClassificationResult = classify_texture(src.original_name, src.relative_hint, image=None)
    if pre_cls.category == "ui_skip":
        return (
            None,
            pre_cls,
            {
                "key": src.key,
                "original_name": src.original_name,
                "source_path": src.source_path,
                "archive_member": src.archive_member,
                "category": "ui_skip",
                "sub_profile": "",
                "confidence": pre_cls.confidence,
                "reasons": list(pre_cls.reasons),
                "status": "skipped_ui",
                "elapsed_ms": round((time.monotonic() - t0) * 1000.0, 2),
            },
            t0,
        )

    try:
        img = load_texture_source(src)
    except Exception as exc:
        return (
            None,
            pre_cls,
            {
                "key": src.key,
                "original_name": src.original_name,
                "source_path": src.source_path,
                "archive_member": src.archive_member,
                "category": pre_cls.category,
                "sub_profile": pre_cls.sub_profile,
                "status": "error",
                "error": str(exc),
                "elapsed_ms": round((time.monotonic() - t0) * 1000.0, 2),
            },
            t0,
        )

    cls = classify_texture(src.original_name, src.relative_hint, image=img)
    record: Dict[str, object] = {
        "key": src.key,
        "original_name": src.original_name,
        "ext": src.ext,
        "source_path": src.source_path,
        "archive_member": src.archive_member,
        "src_size": [img.width, img.height],
        "category": cls.category,
        "sub_profile": cls.sub_profile,
        "confidence": cls.confidence,
        "seamless": cls.seamless,
        "has_alpha_cutout": cls.has_alpha_cutout,
        "reasons": list(cls.reasons),
    }

    if cls.category == "ui_skip":
        record["status"] = "skipped_ui"
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return None, cls, record, t0

    if job.allowed_categories is not None and cls.category not in job.allowed_categories:
        record["status"] = "filtered_out"
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return None, cls, record, t0

    out_dir = Path(job.output_dir)
    albedo_path = out_dir / f"{src.key}.{_output_ext(job.output_format)}"

    if job.dry_run:
        eff_max = compute_effective_max_size(
            cls.category,
            sub_profile=cls.sub_profile,
            max_size=job.max_size,
            hero_max_size=job.hero_max_size,
        )
        prefer_4x = (
            max(256, eff_max // 4)
            if (job.hero_max_size is not None and is_hero_asset(cls.category, cls.sub_profile))
            else 256
        )
        dst_w, dst_h = compute_target_size(
            img.width,
            img.height,
            scale=job.scale,
            max_size=eff_max,
            prefer_4x_up_to=prefer_4x,
        )
        record["dst_size"] = [dst_w, dst_h]
        record["would_write_maps"] = CATEGORY_PROFILES[cls.category].generate_material_maps
        record["output_format"] = job.output_format
        record["status"] = "dry_run"
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return None, cls, record, t0

    if not job.overwrite and albedo_path.exists():
        record["status"] = "exists_skipped"
        record["outputs"] = {"albedo": str(albedo_path)}
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return None, cls, record, t0

    if job.originals_dir:
        save_png(img, Path(job.originals_dir) / f"{src.key}.png")

    return img, cls, None, t0


def _build_base_record(src: TextureSource, img: RGBAImage, cls: ClassificationResult) -> Dict[str, object]:
    return {
        "key": src.key,
        "original_name": src.original_name,
        "ext": src.ext,
        "source_path": src.source_path,
        "archive_member": src.archive_member,
        "src_size": [img.width, img.height],
        "category": cls.category,
        "sub_profile": cls.sub_profile,
        "confidence": cls.confidence,
        "seamless": cls.seamless,
        "has_alpha_cutout": cls.has_alpha_cutout,
        "reasons": list(cls.reasons),
    }


def _run_worker_job(job: _WorkerJob) -> Dict[str, object]:
    """CPU worker entrypoint (must stay top-level for multiprocessing pickling)."""
    img, cls, early, t0 = _decode_and_classify(job)
    if early is not None or img is None or cls is None:
        assert early is not None
        return early

    src = job.source
    record = _build_base_record(src, img, cls)

    try:
        result = process_texture(
            img,
            category=cls.category,
            seamless=cls.seamless,
            has_alpha_cutout=cls.has_alpha_cutout,
            scale=job.scale,
            max_size=job.max_size,
            sub_profile=cls.sub_profile,
            hero_max_size=job.hero_max_size,
            upscaler=job.upscaler,
            normal_backend=job.normal_backend,
            ai_model=job.ai_model,
            deepbump_model=job.deepbump_model,
        )
        if result is None:
            record["status"] = "skipped_ui"
            record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
            return record

        written = save_upscale_result(
            result,
            Path(job.output_dir),
            src.key,
            output_format=job.output_format,
            bc7_quality=job.bc7_quality,
            bc7_backend=job.bc7_backend,
            bc7_threads=job.bc7_threads,
        )
        record["dst_size"] = list(result.dst_size)
        record["upscaler"] = result.upscaler_used
        record["normal_backend"] = result.normal_backend_used
        record["output_format"] = job.output_format
        record["outputs"] = written
        record["status"] = "processed"
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return record
    except Exception as exc:
        record["status"] = "error"
        record["error"] = str(exc)
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return record


def _save_job_result(
    record: Dict[str, object],
    result,
    job: _WorkerJob,
    t0: float,
) -> Dict[str, object]:
    """Thread-pool helper: encode BC7/PNG and write files while GPU processes the next texture."""
    try:
        written = save_upscale_result(
            result,
            Path(job.output_dir),
            job.source.key,
            output_format=job.output_format,
            bc7_quality=job.bc7_quality,
            bc7_backend=job.bc7_backend,
            bc7_threads=job.bc7_threads,
        )
        record["dst_size"] = list(result.dst_size)
        record["upscaler"] = result.upscaler_used
        record["normal_backend"] = result.normal_backend_used
        record["output_format"] = job.output_format
        record["outputs"] = written
        record["status"] = "processed"
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return record
    except Exception as exc:
        record["status"] = "error"
        record["error"] = str(exc)
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return record


def deduplicate_sources(sources: Sequence[TextureSource]) -> Tuple[List[TextureSource], Dict[str, List[str]]]:
    """Deduplicate textures sharing the same canonical override key.

    Matches `CTexture::FindLoadTexture` (game/Texture.cpp:35-40) by preferring
    `.dds` over `.tga` when both exist for the same key. Returns (unique_sources,
    collisions_map).
    """
    chosen: Dict[str, TextureSource] = {}
    collisions: Dict[str, List[str]] = {}

    for src in sources:
        prev = chosen.get(src.key)
        if prev is None:
            chosen[src.key] = src
        else:
            collisions.setdefault(src.key, [prev.relative_hint]).append(src.relative_hint)
            # Prefer .dds over .tga (matches CMR2's load order), otherwise keep first
            if prev.ext != ".dds" and src.ext == ".dds":
                chosen[src.key] = src

    return list(chosen.values()), collisions


def parse_categories_arg(raw_values: Optional[Sequence[str]]) -> Optional[Tuple[str, ...]]:
    """Parse comma-separated or repeated --categories arguments."""
    if not raw_values:
        return None
    selected: List[str] = []
    seen: Set[str] = set()
    for item in raw_values:
        for part in item.split(","):
            cat = part.strip().lower()
            if not cat:
                continue
            if cat not in CATEGORIES:
                raise ValueError(f"Invalid category '{cat}'. Valid categories: {', '.join(CATEGORIES)}")
            if cat not in seen:
                selected.append(cat)
                seen.add(cat)
    return tuple(selected) if selected else None


def run_pipeline(
    data_path: Path,
    output_dir: Path,
    limit: Optional[int] = None,
    workers: int = 1,
    categories: Optional[Tuple[str, ...]] = None,
    dry_run: bool = False,
    manifest_path: Optional[Path] = None,
    scale: Optional[int] = None,
    max_size: int = 2048,
    overwrite: bool = False,
    extract_originals: Optional[Path] = None,
    quiet: bool = False,
    output_format: str = "png",
    upscaler: str = "lanczos",
    normal_backend: str = "scharr",
    ai_model: Optional[Path] = None,
    deepbump_model: Optional[Path] = None,
    bc7_quality: str = "fast",
    bc7_backend: str = "auto",
    hero_max_size: Optional[int] = None,
) -> Dict[str, object]:
    """Execute the complete Stage 10 HD texture pipeline and return a summary dict.

    Default arguments preserve 100% backward compatibility with Stage 9 API callers
    and pure-stdlib unit tests (`output_format="png"`, `upscaler="lanczos"`,
    `normal_backend="scharr"`).
    """
    fmt = (output_format or "png").lower()
    if fmt not in OUTPUT_FORMAT_CHOICES:
        raise ValueError(f"Invalid output format '{output_format}' (expected one of {OUTPUT_FORMAT_CHOICES})")

    resolved_upscaler = resolve_upscaler_backend(upscaler, ai_model=ai_model)
    resolved_normal = resolve_normal_backend(normal_backend, deepbump_model=deepbump_model)
    uses_gpu_or_neural = (resolved_upscaler == "mps_esrgan") or (resolved_normal in ("deepbump", "hybrid"))

    t_start = time.monotonic()
    sources, warnings = scan_data_directory(data_path)
    unique_sources, collisions = deduplicate_sources(sources)

    if limit is not None:
        unique_sources = unique_sources[: max(0, limit)]

    if not quiet:
        print("OpenCMR2 Stage 10 HD Texture Pipeline")
        print(f"  Image I/O : {backend_summary()}")
        print(f"  Upscaler  : {upscaler_backend_summary(upscaler, ai_model=ai_model)}")
        print(f"  Normals   : {normal_backend_summary(normal_backend, deepbump_model=deepbump_model)}")
        if fmt in ("dds", "bc7"):
            print(f"  Format    : DX10 BC7 DDS (quality={bc7_quality}, {bc7_backend_summary()})")
        else:
            print("  Format    : PNG")
        print(f"  Data      : {data_path}")
        print(f"  Output    : {output_dir}{' (DRY RUN)' if dry_run else ''}")
        if hero_max_size is not None and hero_max_size != max_size:
            print(f"  Sizes     : max={max_size}px, hero_max={hero_max_size}px")
        print(f"  Found     : {len(sources)} textures ({len(unique_sources)} unique keys to inspect)")
        if categories:
            print(f"  Categories: {', '.join(categories)}")
        if warnings:
            print(f"  Warnings  : {len(warnings)} archive read warning(s)")

    worker_count = max(1, int(workers))
    cpu_total = max(1, os.cpu_count() or 1)
    # Avoid nested thread oversubscription when running multiple workers with C++ multithreaded BC7
    if uses_gpu_or_neural:
        io_workers = min(4, max(1, worker_count))
        per_job_bc7_threads = max(1, cpu_total // max(1, io_workers))
    else:
        io_workers = worker_count
        per_job_bc7_threads = max(1, cpu_total // max(1, worker_count))

    jobs = [
        _WorkerJob(
            source=src,
            output_dir=str(output_dir),
            allowed_categories=categories,
            scale=scale,
            max_size=max_size,
            hero_max_size=hero_max_size,
            dry_run=dry_run,
            overwrite=overwrite,
            originals_dir=str(extract_originals) if extract_originals else None,
            output_format=fmt,
            upscaler=resolved_upscaler,
            normal_backend=resolved_normal,
            ai_model=str(ai_model) if ai_model else None,
            deepbump_model=str(deepbump_model) if deepbump_model else None,
            bc7_quality=bc7_quality,
            bc7_backend=bc7_backend,
            bc7_threads=per_job_bc7_threads,
        )
        for src in unique_sources
    ]

    records: List[Dict[str, object]] = []
    total = len(jobs)
    reported = 0

    def _report(rec: Dict[str, object]) -> None:
        nonlocal reported
        reported += 1
        if quiet:
            return
        status = rec["status"]
        cat = rec.get("category", "?")
        sub = rec.get("sub_profile", "")
        cat_label = f"{cat}/{sub}" if sub else str(cat)
        key = rec["key"]
        dims = ""
        if "src_size" in rec and "dst_size" in rec:
            s = rec["src_size"]
            d = rec["dst_size"]
            dims = f" {s[0]}x{s[1]}->{d[0]}x{d[1]}"
        err = f" ERR={rec['error']}" if "error" in rec else ""
        print(f"[{reported:4d}/{total:4d}] {status:14s} [{cat_label:24s}] {key}{dims}{err}")

    if uses_gpu_or_neural and not dry_run and total > 0:
        # Staged Apple Silicon GPU + CPU pipeline:
        #   - ThreadPool decodes BFL/DDS/TGA + classifies ahead of the GPU
        #   - Main thread executes MPS ESRGAN & DeepBump256 serially (zero GPU contention / no fork)
        #   - ThreadPool compresses BC7/PNG & writes files concurrently (C++ bc7enc releases GIL!)
        prefetch_depth = max(2, io_workers * 2)
        save_depth = max(2, io_workers)

        with ThreadPoolExecutor(max_workers=io_workers) as decode_pool, ThreadPoolExecutor(
            max_workers=io_workers
        ) as save_pool:
            decode_queue: Deque[Tuple[_WorkerJob, Future]] = deque()
            save_queue: Deque[Future] = deque()

            job_iter = iter(jobs)
            for _ in range(min(prefetch_depth, total)):
                j = next(job_iter)
                decode_queue.append((j, decode_pool.submit(_decode_and_classify, j)))

            while decode_queue:
                job, fut = decode_queue.popleft()
                # Keep prefetch queue topped up
                try:
                    next_job = next(job_iter)
                    decode_queue.append((next_job, decode_pool.submit(_decode_and_classify, next_job)))
                except StopIteration:
                    pass

                img, cls, early, t0 = fut.result()
                if early is not None or img is None or cls is None:
                    assert early is not None
                    records.append(early)
                    _report(early)
                    continue

                record = _build_base_record(job.source, img, cls)
                try:
                    result = process_texture(
                        img,
                        category=cls.category,
                        seamless=cls.seamless,
                        has_alpha_cutout=cls.has_alpha_cutout,
                        scale=job.scale,
                        max_size=job.max_size,
                        sub_profile=cls.sub_profile,
                        hero_max_size=job.hero_max_size,
                        upscaler=job.upscaler,
                        normal_backend=job.normal_backend,
                        ai_model=job.ai_model,
                        deepbump_model=job.deepbump_model,
                    )
                except Exception as exc:
                    record["status"] = "error"
                    record["error"] = str(exc)
                    record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
                    records.append(record)
                    _report(record)
                    continue

                if result is None:
                    record["status"] = "skipped_ui"
                    record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
                    records.append(record)
                    _report(record)
                    continue

                save_queue.append(save_pool.submit(_save_job_result, record, result, job, t0))
                while len(save_queue) >= save_depth:
                    done_rec = save_queue.popleft().result()
                    records.append(done_rec)
                    _report(done_rec)

            while save_queue:
                done_rec = save_queue.popleft().result()
                records.append(done_rec)
                _report(done_rec)

        records.sort(key=lambda r: str(r["key"]))
    elif worker_count == 1 or total <= 1:
        for job in jobs:
            rec = _run_worker_job(job)
            records.append(rec)
            _report(rec)
    else:
        with ProcessPoolExecutor(max_workers=worker_count) as pool:
            future_map = {pool.submit(_run_worker_job, job): job.source.key for job in jobs}
            for fut in as_completed(future_map):
                rec = fut.result()
                records.append(rec)
                _report(rec)
        records.sort(key=lambda r: str(r["key"]))

    category_counts = {c: 0 for c in CATEGORIES}
    status_counts: Dict[str, int] = {}
    for rec in records:
        cat = str(rec.get("category", ""))
        if cat in category_counts:
            category_counts[cat] += 1
        st = str(rec.get("status", "unknown"))
        status_counts[st] = status_counts.get(st, 0) + 1

    ext = _output_ext(fmt)
    elapsed_s = round(time.monotonic() - t_start, 3)
    manifest: Dict[str, object] = {
        "pipeline": "OpenCMR2 Stage 10 HD Texture Pipeline",
        "backend": backend_summary(),
        "upscaler": upscaler_backend_summary(upscaler, ai_model=ai_model),
        "normal_backend": normal_backend_summary(normal_backend, deepbump_model=deepbump_model),
        "output_format": fmt,
        "bc7_quality": bc7_quality if fmt in ("dds", "bc7") else None,
        "bc7_backend": bc7_backend_summary() if fmt in ("dds", "bc7") else None,
        "max_size": max_size,
        "hero_max_size": hero_max_size,
        "data_path": str(data_path),
        "output_dir": str(output_dir),
        "dry_run": dry_run,
        "naming_convention": f"overrides/<lowercase_stem>.{ext} (+ _n.{ext} normal, _s.{ext} specular)",
        "total_discovered": len(sources),
        "total_unique_processed": len(records),
        "elapsed_seconds": elapsed_s,
        "category_counts": category_counts,
        "status_counts": status_counts,
        "warnings": warnings,
        "key_collisions": collisions,
        "textures": records,
    }

    target_manifest = manifest_path
    if target_manifest is None and not dry_run:
        target_manifest = output_dir / "manifest.json"
    if target_manifest is not None:
        target_manifest.parent.mkdir(parents=True, exist_ok=True)
        target_manifest.write_text(json.dumps(manifest, indent=2), encoding="utf-8")

    if not quiet:
        print(f"\nCompleted in {elapsed_s:.2f}s:")
        print(f"  Status    : {status_counts}")
        print(f"  Categories: {category_counts}")
        if target_manifest is not None:
            print(f"  Manifest  : {target_manifest}")

    return manifest


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=(
            "OpenCMR2 Stage 10 HD Texture Pipeline "
            "(BFL extract -> classify -> AI/Lanczos upscale + neural _n/_s maps -> PNG or DX10 BC7 DDS)."
        )
    )
    parser.add_argument("--data", required=True, help="Path to CMR2 game data directory or single .BFL archive")
    parser.add_argument("--output", required=True, help="Target OpenCMR2 overrides/ directory")
    parser.add_argument("--limit", type=int, default=None, help="Maximum number of unique textures to process")
    parser.add_argument(
        "--workers",
        type=int,
        default=max(1, os.cpu_count() or 1),
        help="Number of parallel CPU workers (default: CPU count)",
    )
    parser.add_argument(
        "--categories",
        action="append",
        default=None,
        help="Filter to specific categories (comma-separated or repeated), e.g. road_surface,car_livery",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Scan, decode, and classify without writing any output textures",
    )
    parser.add_argument(
        "--manifest",
        default=None,
        help="Path to write JSON manifest (default: <output>/manifest.json unless --dry-run)",
    )
    parser.add_argument("--scale", type=int, default=None, help="Explicit upscale factor (2 or 4; default: auto)")
    parser.add_argument("--max-size", type=int, default=2048, help="Maximum output dimension in px (default: 2048)")
    parser.add_argument(
        "--hero-max-size",
        type=int,
        default=2048,
        help="Maximum output dimension in px for hero car_livery & road_surface assets (default: 2048; e.g. 4096)",
    )
    parser.add_argument(
        "--format",
        dest="output_format",
        choices=OUTPUT_FORMAT_CHOICES,
        default="png",
        help="Output texture format: png or DX10 BC7 dds/bc7 with full mipmaps (default: png)",
    )
    parser.add_argument(
        "--upscaler",
        choices=UPSCALER_BACKENDS,
        default="auto",
        help="Super-resolution backend: auto, mps_esrgan, ncnn, lanczos (default: auto)",
    )
    parser.add_argument(
        "--normal-backend",
        choices=NORMAL_BACKENDS,
        default="auto",
        help="Normal map generator: auto, deepbump, hybrid, scharr (default: auto)",
    )
    parser.add_argument(
        "--ai-model",
        default=None,
        help="Path to ESRGAN 4x .pth checkpoint (default: ~/.cache/opencmr2_hd/models/4x-UltraSharp.pth)",
    )
    parser.add_argument(
        "--deepbump-model",
        default=None,
        help="Path to deepbump256.onnx model (default: ~/.cache/opencmr2_hd/models/deepbump256.onnx)",
    )
    parser.add_argument(
        "--bc7-quality",
        choices=BC7_QUALITY_CHOICES,
        default="fast",
        help="BC7 compression quality preset: ultra_fast, fast, medium, high (default: fast)",
    )
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing textures in output directory")
    parser.add_argument(
        "--extract-originals",
        default=None,
        help="Optional directory to also save unmodified decoded original PNGs for A/B comparison",
    )
    parser.add_argument("--quiet", action="store_true", help="Suppress per-texture progress lines")
    args = parser.parse_args(list(argv) if argv is not None else None)

    categories = parse_categories_arg(args.categories)
    run_pipeline(
        data_path=Path(args.data),
        output_dir=Path(args.output),
        limit=args.limit,
        workers=args.workers,
        categories=categories,
        dry_run=args.dry_run,
        manifest_path=Path(args.manifest) if args.manifest else None,
        scale=args.scale,
        max_size=args.max_size,
        overwrite=args.overwrite,
        extract_originals=Path(args.extract_originals) if args.extract_originals else None,
        quiet=args.quiet,
        output_format=args.output_format,
        upscaler=args.upscaler,
        normal_backend=args.normal_backend,
        ai_model=Path(args.ai_model) if args.ai_model else None,
        deepbump_model=Path(args.deepbump_model) if args.deepbump_model else None,
        bc7_quality=args.bc7_quality,
        hero_max_size=args.hero_max_size,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

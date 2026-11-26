#!/usr/bin/env python3
"""Stage 9 HD Texture Pipeline CLI for Colin McRae Rally 2.0 (OpenCMR2).

Orchestrates:
  1. Read-only scan of CMR2 `.BFL` archives and loose `.DDS`/`.TGA` textures
  2. Automatic texture classification (and automatic skip of OpenCMR2 vectorized
     UI/font textures)
  3. Parallel HD upscaling (2x/4x up to 2048px) with periodic wrap tiling,
     alpha-cutout RGB color bleeding, and paired `_n.png` + `_s.png` generation
     directly into an OpenCMR2 `overrides/` directory.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from concurrent.futures import ProcessPoolExecutor, as_completed
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from _image_io import backend_summary, save_png
    from bfl_extract import TextureSource, load_texture_source, scan_data_directory
    from classify_textures import CATEGORIES, ClassificationResult, classify_texture
    from upscale_hd import process_texture, save_upscale_result
else:
    from ._image_io import backend_summary, save_png
    from .bfl_extract import TextureSource, load_texture_source, scan_data_directory
    from .classify_textures import CATEGORIES, ClassificationResult, classify_texture
    from .upscale_hd import process_texture, save_upscale_result


@dataclass(frozen=True)
class _WorkerJob:
    source: TextureSource
    output_dir: str
    allowed_categories: Optional[Tuple[str, ...]]
    scale: Optional[int]
    max_size: int
    dry_run: bool
    overwrite: bool
    originals_dir: Optional[str]


def _run_worker_job(job: _WorkerJob) -> Dict[str, object]:
    """Worker entrypoint (must stay top-level for multiprocessing pickling)."""
    src = job.source
    t0 = time.monotonic()

    # Fast pre-check for exact UI/font skips before decoding pixels
    pre_cls: ClassificationResult = classify_texture(src.original_name, src.relative_hint, image=None)
    if pre_cls.category == "ui_skip":
        return {
            "key": src.key,
            "original_name": src.original_name,
            "source_path": src.source_path,
            "archive_member": src.archive_member,
            "category": "ui_skip",
            "confidence": pre_cls.confidence,
            "reasons": list(pre_cls.reasons),
            "status": "skipped_ui",
            "elapsed_ms": round((time.monotonic() - t0) * 1000.0, 2),
        }

    try:
        img = load_texture_source(src)
    except Exception as exc:
        return {
            "key": src.key,
            "original_name": src.original_name,
            "source_path": src.source_path,
            "archive_member": src.archive_member,
            "category": pre_cls.category,
            "status": "error",
            "error": str(exc),
            "elapsed_ms": round((time.monotonic() - t0) * 1000.0, 2),
        }

    cls = classify_texture(src.original_name, src.relative_hint, image=img)
    record: Dict[str, object] = {
        "key": src.key,
        "original_name": src.original_name,
        "ext": src.ext,
        "source_path": src.source_path,
        "archive_member": src.archive_member,
        "src_size": [img.width, img.height],
        "category": cls.category,
        "confidence": cls.confidence,
        "seamless": cls.seamless,
        "has_alpha_cutout": cls.has_alpha_cutout,
        "reasons": list(cls.reasons),
    }

    if cls.category == "ui_skip":
        record["status"] = "skipped_ui"
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return record

    if job.allowed_categories is not None and cls.category not in job.allowed_categories:
        record["status"] = "filtered_out"
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return record

    out_dir = Path(job.output_dir)
    albedo_path = out_dir / f"{src.key}.png"

    if job.dry_run:
        from upscale_hd import CATEGORY_PROFILES, compute_target_size

        dst_w, dst_h = compute_target_size(img.width, img.height, scale=job.scale, max_size=job.max_size)
        record["dst_size"] = [dst_w, dst_h]
        record["would_write_maps"] = CATEGORY_PROFILES[cls.category].generate_material_maps
        record["status"] = "dry_run"
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return record

    if not job.overwrite and albedo_path.exists():
        record["status"] = "exists_skipped"
        record["outputs"] = {"albedo": str(albedo_path)}
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return record

    if job.originals_dir:
        save_png(img, Path(job.originals_dir) / f"{src.key}.png")

    result = process_texture(
        img,
        category=cls.category,
        seamless=cls.seamless,
        has_alpha_cutout=cls.has_alpha_cutout,
        scale=job.scale,
        max_size=job.max_size,
    )
    if result is None:
        record["status"] = "skipped_ui"
        record["elapsed_ms"] = round((time.monotonic() - t0) * 1000.0, 2)
        return record

    written = save_upscale_result(result, out_dir, src.key)
    record["dst_size"] = list(result.dst_size)
    record["outputs"] = written
    record["status"] = "processed"
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
) -> Dict[str, object]:
    """Execute the complete Stage 9 HD texture pipeline and return a summary dict."""
    t_start = time.monotonic()
    sources, warnings = scan_data_directory(data_path)
    unique_sources, collisions = deduplicate_sources(sources)

    if limit is not None:
        unique_sources = unique_sources[: max(0, limit)]

    if not quiet:
        print(f"OpenCMR2 Stage 9 HD Texture Pipeline")
        print(f"  Backend   : {backend_summary()}")
        print(f"  Data      : {data_path}")
        print(f"  Output    : {output_dir}{' (DRY RUN)' if dry_run else ''}")
        print(f"  Found     : {len(sources)} textures ({len(unique_sources)} unique keys to inspect)")
        if categories:
            print(f"  Categories: {', '.join(categories)}")
        if warnings:
            print(f"  Warnings  : {len(warnings)} archive read warning(s)")

    jobs = [
        _WorkerJob(
            source=src,
            output_dir=str(output_dir),
            allowed_categories=categories,
            scale=scale,
            max_size=max_size,
            dry_run=dry_run,
            overwrite=overwrite,
            originals_dir=str(extract_originals) if extract_originals else None,
        )
        for src in unique_sources
    ]

    records: List[Dict[str, object]] = []
    total = len(jobs)

    def _report(idx: int, rec: Dict[str, object]) -> None:
        if quiet:
            return
        status = rec["status"]
        cat = rec.get("category", "?")
        key = rec["key"]
        dims = ""
        if "src_size" in rec and "dst_size" in rec:
            s = rec["src_size"]
            d = rec["dst_size"]
            dims = f" {s[0]}x{s[1]}->{d[0]}x{d[1]}"
        err = f" ERR={rec['error']}" if "error" in rec else ""
        print(f"[{idx:4d}/{total:4d}] {status:14s} [{cat:15s}] {key}{dims}{err}")

    worker_count = max(1, int(workers))
    if worker_count == 1 or total <= 1:
        for i, job in enumerate(jobs, start=1):
            rec = _run_worker_job(job)
            records.append(rec)
            _report(i, rec)
    else:
        with ProcessPoolExecutor(max_workers=worker_count) as pool:
            future_map = {pool.submit(_run_worker_job, job): job.source.key for job in jobs}
            for i, fut in enumerate(as_completed(future_map), start=1):
                rec = fut.result()
                records.append(rec)
                _report(i, rec)
        records.sort(key=lambda r: str(r["key"]))

    category_counts = {c: 0 for c in CATEGORIES}
    status_counts: Dict[str, int] = {}
    for rec in records:
        cat = str(rec.get("category", ""))
        if cat in category_counts:
            category_counts[cat] += 1
        st = str(rec.get("status", "unknown"))
        status_counts[st] = status_counts.get(st, 0) + 1

    elapsed_s = round(time.monotonic() - t_start, 3)
    manifest: Dict[str, object] = {
        "pipeline": "OpenCMR2 Stage 9 HD Texture Pipeline",
        "backend": backend_summary(),
        "data_path": str(data_path),
        "output_dir": str(output_dir),
        "dry_run": dry_run,
        "naming_convention": "overrides/<lowercase_stem>.png (+ _n.png normal, _s.png specular)",
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
        description="OpenCMR2 Stage 9 HD Texture Pipeline (BFL extract -> classify -> HD upscale + _n/_s maps)."
    )
    parser.add_argument("--data", required=True, help="Path to CMR2 game data directory or single .BFL archive")
    parser.add_argument("--output", required=True, help="Target OpenCMR2 overrides/ directory")
    parser.add_argument("--limit", type=int, default=None, help="Maximum number of unique textures to process")
    parser.add_argument(
        "--workers",
        type=int,
        default=max(1, os.cpu_count() or 1),
        help="Number of parallel worker processes (default: CPU count)",
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
        help="Scan, decode, and classify without writing any PNG textures",
    )
    parser.add_argument(
        "--manifest",
        default=None,
        help="Path to write JSON manifest (default: <output>/manifest.json unless --dry-run)",
    )
    parser.add_argument("--scale", type=int, default=None, help="Explicit upscale factor (2 or 4; default: auto)")
    parser.add_argument("--max-size", type=int, default=2048, help="Maximum output dimension in px (default: 2048)")
    parser.add_argument("--overwrite", action="store_true", help="Overwrite existing PNGs in output directory")
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
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

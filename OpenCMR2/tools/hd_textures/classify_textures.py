#!/usr/bin/env python3
"""Classify Colin McRae Rally 2.0 textures for the Stage 9 HD overhaul pipeline.

Categories:
  - road_surface    : asphalt, gravel, tarmac, snow, mud, sand, grass (tiled/seamless)
  - car_livery      : car bodies (*bodf), liveries, wheels, interior, glass (UV 1:1)
  - foliage_alpha   : trees, bushes, fences, crowds, signs with alpha cutouts
  - buildings_props : buildings, walls, barriers, billboards, banners, rocks
  - sky_backdrop    : skyboxes, clouds, distant horizons, environment maps
  - ui_skip         : fonts (hel_*, lcd_*), vectorized OpenCMR2 icons/maps
                      (ar_640*, smmatrix, arrows4, cha01..08, tick, cross, box,
                      circle, chalblob), dots, HUD/frontend chrome (untouched)
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable, List, Optional, Tuple

if __package__ in (None, ""):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from _image_io import RGBAImage, load_png
    from bfl_extract import normalize_texture_key, scan_data_directory
else:
    from ._image_io import RGBAImage, load_png
    from .bfl_extract import normalize_texture_key, scan_data_directory

CATEGORIES = (
    "road_surface",
    "car_livery",
    "foliage_alpha",
    "buildings_props",
    "sky_backdrop",
    "ui_skip",
)

SUB_PROFILES = (
    "car_body",
    "car_cockpit",
    "car_glass",
    "car_wheel",
    "car_light",
    "road_tarmac",
    "road_gravel",
    "road_snow",
)

# Exact textures vectorized at runtime by OpenCMR2:
#   - src/render/vector_sprite.cpp (Cells / IsMap / IsArcadeMap)
#   - src/render/outline_font.cpp + game/Font.cpp (hel_*, lcd_*, etc.)
#   - game/Frontend.cpp, game/FrontendScreens.cpp, game/HudDash.cpp
VECTORIZED_UI_EXACT = frozenset(
    {
        # vector_sprite.cpp maps & icons
        "cha01",
        "cha02",
        "cha03",
        "cha04",
        "cha05",
        "cha06",
        "cha07",
        "cha08",
        "smmatrix",
        "lgmatrix",
        "chalblob",
        "netblob",
        "netblobr",
        "arrows4",
        "ar_640a",
        "ar_640d",
        "arrow",
        "tick",
        "cross",
        "box",
        "circle",
        # frontend / HUD chrome & icons
        "dot",
        "bink",
        "cmr2",
        "danred",
        "danora",
        "danyel",
        "tbronze",
        "tsilver",
        "tgold",
        "mainmap",
        "rev_640",
        "revdigital",
        "kph",
        "mph",
        "lgt_red",
        "lgt_gre",
    }
)

UI_PREFIXES = (
    "hel_",
    "lcd_",
    "dot0",
    "ar_640",
    "ar_1024",
    "crtna",
    "rocx_",
    "copyright",
    "font",
    "fnt_",
    "hud_",
    "osd_",
    "menu_",
    "ui_",
    "btn_",
    "icon_",
    "flag_",
    "tinyflag",
    "cursor",
    "splash",
)

UI_SUFFIXES = (
    "digit",  # Graphics.cpp: m_strSuffixDIGIT
    "revct",  # Graphics.cpp: m_strSuffixREVCT
    "_font",
    "_hud",
    "_ui",
    "_icon",
)

UI_ARCHIVE_HINTS = (
    "res640",
    "res1024",
    "feres640",
    "feres1024",
    "englishtext",
    "frenchtext",
    "germantext",
    "spanishtext",
    "italiantext",
    "polishtext",
    "engusatext",
    "dmd.bfl",
    "/fonts/",
    "/osd/",
    "/menus/",
    "/symbols/",
    "/tinyflags/",
    "/setuprep/",
    "/weather/",
)

# Keyword patterns per 3D world category
SKY_PATTERNS = re.compile(
    r"(^|[_\-])(sky|skydome|skybox|cloud|clouds|horizon|horiz|sun|sunglow|sunset|sunrise|"
    r"backdrop|backdrp|distant|distmount|farhill|environment|envmap|env_|cubemap|overcast|haze)"
)

_CAR_PREFIX = (
    r"(?:ford|foc|focus|sub|subaru|imp|impreza|peug|peu|206|mits|mit|evo|lan|lancer|"
    r"seat|cord|cor|coro|corolla|toy|toyota|del|delta|strat|stratos|esc|escort|mini|"
    r"sier|sierra|met|metro|6r4|rs200|quat|quattro|cel|celica|intg|integrale|car|veh)"
)

CAR_PATTERNS = re.compile(
    r"(boddf$|dbodf$|bodf$|glid|glod|glob|livery|car\d|chassis|bonnet|hood|bumper|spoiler|wing|"
    r"wheel|tyre|tire|rim|alloy|cockpit|interior|int_|dash|steer|seat|rollcage|windscr|windsh|"
    r"glass|headl|headlite|taill|brkelite|revlite|hazdlite|mirror|exhaust|grille|numberplate|"
    r"plate|driver|helmet|co_driv|codriv|"
    r"subaru|impreza|focus|ford|lancer|evo|peugeot|206|cordoba|corolla|mitsu|delta|stratos|"
    r"escort|mini|sierra|metro|6r4|rs200|quattro|celica|integrale|paint$|"
    rf"^{_CAR_PREFIX}[a-z0-9_]*?(?:boddf|dbodf|bodf|glid|glod|glob|int|und|wh|lig)\d*$)"
)

CAR_ARCHIVE_HINTS = (
    "/cars/",
    "c5.bfl",
    "a5.bfl",
    "a1.bfl",
    "/interior/",
)

# Sub-profile matchers (evaluated after top-level category is determined)
_CAR_BODY_SUB_RE = re.compile(r"(boddf|dbodf|bodf|body|livery|bonnet|hood|bumper|spoiler|wing|paint$)")
_CAR_GLASS_SUB_RE = re.compile(r"(glid|glod|glob|windscr|windsh|glass|window)")
_CAR_LIGHT_SUB_RE = re.compile(
    rf"(headl|headlite|taill|brkelite|revlite|hazdlite|fogl|light|lamp|(?:^|[_\-])lig|^{_CAR_PREFIX}[a-z0-9_]*?lig\d*$)"
)
_CAR_WHEEL_SUB_RE = re.compile(
    rf"(wheel|tyre|tyr|tire|rim|alloy|brake|disc|calip|underbody|under_|(?:^|[_\-])(?:und|wh\d*)|"
    rf"^{_CAR_PREFIX}[a-z0-9_]*?(?:und|wh)\d*$)"
)
_CAR_COCKPIT_STRONG_RE = re.compile(
    rf"(cockpit|interior|dash|steer|rollcage|cabin|dial|gauge|speedo|"
    rf"(?:^|[_\-])int(?:[_\-\d]|$)|^{_CAR_PREFIX}[a-z0-9_]*?int\d*$|/interior/)"
)
_CAR_SEAT_SUB_RE = re.compile(r"(?:^|[_\-])seat|seat[_\-]|seats")

_ROAD_SNOW_SUB_RE = re.compile(r"(snow|ice|icy|frost|slush|winter|swed)")
_ROAD_GRAVEL_SUB_RE = re.compile(
    r"(gravel|grav|grvl|dirt|mud|sand|sandy|soil|earth|clay|dust|shingle|pebble|"
    r"grass|turf|verge|bank|meadow|moss|field|heath|trail)"
)
_ROAD_TARMAC_SUB_RE = re.compile(r"(tarmac|tarm|asph|asphalt|cobble|pave|paving|curb|kerb|skid|wet_|puddle)")

ROAD_PATTERNS = re.compile(
    r"(road|tarmac|tarm|asph|asphalt|gravel|grav|grvl|dirt|mud|snow|ice|icy|sand|sandy|"
    r"grass|turf|verge|bank|curb|kerb|cobble|pave|paving|track|surf|surface|ground|soil|"
    r"earth|lane|skid|rut|ruts|puddle|wet_|slush|frost|dust|field|meadow|moss|heath|"
    r"shingle|pebble|clay|trail|path)"
)

FOLIAGE_PATTERNS = re.compile(
    r"(tree|trees|pine|oak|birch|palm|spruce|fir|bush|bushes|shrub|hedge|leaf|leaves|"
    r"foliage|branch|fern|weed|reeds|reed|ivy|vine|plant|flora|forest|wood|woods|canopy|"
    r"fence|fenc|wire|mesh|netting|crowd|crowds|spec|spectat|marshal|marsh|people|person|"
    r"photog|sign|signs|arrow_sign|chevron|tape|ribbon|bunting|flag_post|alpha|cutout|splat|spray)"
)

BUILDING_PATTERNS = re.compile(
    r"(wall|brick|stone|rock|rocks|cliff|boulder|crag|house|home|barn|shed|hut|cabin|"
    r"cottage|bldg|build|building|roof|tile|window|door|garage|bridge|tunnel|arch|pillar|"
    r"column|barrier|armco|guardrail|rail|concrete|cement|plaster|stucco|woodplank|plank|"
    r"timber|log|post|pole|pylon|tower|billboard|board|banner|advert|sponsor|hoarding|"
    r"tent|marquee|awning|crate|box_|barrel|hay|bale|cone|bollard|tyrewall|tirewall|"
    r"lamp|lightpost|chimney|silo|church|castle|ruin|shop|pub|hotel|station|booth|stand|"
    r"grandstand|bleacher|container|truck|van|tractor|caravan|trailer)"
)


@dataclass(frozen=True)
class ImageMetrics:
    """Lightweight pixel metrics used to refine classification and tiling behavior."""

    width: int
    height: int
    has_alpha: bool
    transparent_ratio: float
    semitransparent_ratio: float
    alpha_cutout: bool
    seam_h_diff: float  # mean abs RGB diff between left and right edges [0..255]
    seam_v_diff: float  # mean abs RGB diff between top and bottom edges [0..255]
    looks_seamless: bool
    sky_gradient_score: float  # [0..1]


@dataclass(frozen=True)
class ClassificationResult:
    """Classification output for a single CMR2 texture."""

    key: str
    category: str
    confidence: float
    seamless: bool
    has_alpha_cutout: bool
    reasons: Tuple[str, ...]
    sub_profile: str = ""

    def to_dict(self) -> dict:
        d = asdict(self)
        d["reasons"] = list(self.reasons)
        return d


def analyze_image(image: RGBAImage) -> ImageMetrics:
    """Compute alpha cutout, border seam continuity, and sky-gradient heuristics."""
    w, h = image.width, image.height
    data = image.data
    # Sample a stride so large images stay fast even in pure Python
    step_x = max(1, w // 64)
    step_y = max(1, h // 64)

    total = 0
    trans = 0
    semi = 0
    opaque = 0
    top_blue_bias = 0.0
    top_samples = 0
    bottom_luma = 0.0
    bottom_samples = 0

    for y in range(0, h, step_y):
        row = y * w * 4
        for x in range(0, w, step_x):
            i = row + x * 4
            r, g, b, a = data[i], data[i + 1], data[i + 2], data[i + 3]
            total += 1
            if a < 24:
                trans += 1
            elif a < 232:
                semi += 1
            else:
                opaque += 1
            if y < h // 3:
                top_blue_bias += (b - 0.5 * (r + g)) / 255.0
                top_samples += 1
            elif y >= (2 * h) // 3:
                bottom_luma += (0.299 * r + 0.587 * g + 0.114 * b) / 255.0
                bottom_samples += 1

    total = max(1, total)
    trans_ratio = trans / total
    semi_ratio = semi / total
    has_alpha = (trans + semi) > 0
    alpha_cutout = trans_ratio >= 0.04 and (opaque / total) >= 0.04

    # Check border seam continuity (wrapping left<->right and top<->bottom)
    h_diff_sum = 0.0
    h_count = 0
    for y in range(0, h, max(1, h // 64)):
        li = (y * w) * 4
        ri = (y * w + (w - 1)) * 4
        h_diff_sum += (
            abs(data[li] - data[ri]) + abs(data[li + 1] - data[ri + 1]) + abs(data[li + 2] - data[ri + 2])
        ) / 3.0
        h_count += 1
    seam_h = h_diff_sum / max(1, h_count)

    v_diff_sum = 0.0
    v_count = 0
    bot_row = (h - 1) * w * 4
    for x in range(0, w, max(1, w // 64)):
        ti = x * 4
        bi = bot_row + x * 4
        v_diff_sum += (
            abs(data[ti] - data[bi]) + abs(data[ti + 1] - data[bi + 1]) + abs(data[ti + 2] - data[bi + 2])
        ) / 3.0
        v_count += 1
    seam_v = v_diff_sum / max(1, v_count)

    looks_seamless = (not alpha_cutout) and (seam_h < 22.0) and (seam_v < 22.0)

    sky_score = 0.0
    if not has_alpha and top_samples > 0 and bottom_samples > 0:
        avg_blue_bias = top_blue_bias / top_samples
        avg_bot_luma = bottom_luma / bottom_samples
        if avg_blue_bias > 0.06 and seam_h < 28.0 and avg_bot_luma > 0.35:
            sky_score = min(1.0, avg_blue_bias * 4.0)

    return ImageMetrics(
        width=w,
        height=h,
        has_alpha=has_alpha,
        transparent_ratio=trans_ratio,
        semitransparent_ratio=semi_ratio,
        alpha_cutout=alpha_cutout,
        seam_h_diff=seam_h,
        seam_v_diff=seam_v,
        looks_seamless=looks_seamless,
        sky_gradient_score=sky_score,
    )


def detect_sub_profile(key: str, path_lower: str, category: str) -> str:
    """Determine fine-grained Stage 10 PBR sub-profile for car_livery and road_surface."""
    if category == "car_livery":
        # Check explicit exterior body suffixes FIRST so 'seatbodf' (SEAT Cordoba body)
        # maps to car_body rather than cockpit 'seat'.
        if _CAR_BODY_SUB_RE.search(key):
            return "car_body"
        if _CAR_GLASS_SUB_RE.search(key):
            return "car_glass"
        if _CAR_LIGHT_SUB_RE.search(key):
            return "car_light"
        if _CAR_COCKPIT_STRONG_RE.search(key) or "/interior/" in path_lower:
            return "car_cockpit"
        if _CAR_WHEEL_SUB_RE.search(key):
            return "car_wheel"
        if _CAR_SEAT_SUB_RE.search(key):
            return "car_cockpit"
        return "car_body"

    if category == "road_surface":
        if _ROAD_SNOW_SUB_RE.search(key) or "/swed" in path_lower:
            return "road_snow"
        if _ROAD_GRAVEL_SUB_RE.search(key):
            return "road_gravel"
        if _ROAD_TARMAC_SUB_RE.search(key):
            return "road_tarmac"
        return "road_tarmac"

    return ""


def _is_ui_skip(key: str, path_lower: str) -> Optional[str]:
    if key in VECTORIZED_UI_EXACT:
        return f"exact OpenCMR2 vectorized/UI texture '{key}'"
    for prefix in UI_PREFIXES:
        if key.startswith(prefix):
            return f"UI/font prefix '{prefix}'"
    for suffix in UI_SUFFIXES:
        if key.endswith(suffix):
            return f"UI/HUD suffix '{suffix}'"
    # Frontend car previews should still be allowed if explicitly under frontend/textures/cars/
    if "/frontend/textures/cars/" in path_lower:
        return None
    if "/frontend/" in path_lower:
        return "frontend UI archive/path"
    for hint in UI_ARCHIVE_HINTS:
        if hint in path_lower:
            return f"UI/font archive hint '{hint}'"
    return None


def classify_texture(
    name: str,
    source_hint: str = "",
    image: Optional[RGBAImage] = None,
) -> ClassificationResult:
    """Classify a CMR2 texture into one of the 6 Stage 9 categories."""
    key = normalize_texture_key(name)
    path_lower = f"{source_hint}/{name}".replace("\\", "/").lower()
    reasons: List[str] = []

    metrics = analyze_image(image) if image is not None else None
    has_cutout = metrics.alpha_cutout if metrics is not None else False

    # 1. Hard skip for OpenCMR2 vectorized UI/fonts/HUD chrome
    ui_reason = _is_ui_skip(key, path_lower)
    if ui_reason is not None:
        return ClassificationResult(
            key=key,
            category="ui_skip",
            confidence=0.98,
            seamless=False,
            has_alpha_cutout=has_cutout,
            reasons=(ui_reason,),
        )

    # 2. Score each world category using filename, archive path, and optional pixels
    scores = {
        "road_surface": 0.0,
        "car_livery": 0.0,
        "foliage_alpha": 0.0,
        "buildings_props": 0.0,
        "sky_backdrop": 0.0,
    }

    if SKY_PATTERNS.search(key):
        scores["sky_backdrop"] += 3.2
        reasons.append("sky/horizon keyword match")

    if CAR_PATTERNS.search(key):
        scores["car_livery"] += 3.2
        reasons.append("car/livery keyword match")
    if any(h in path_lower for h in CAR_ARCHIVE_HINTS):
        scores["car_livery"] += 2.2
        reasons.append("car archive/folder hint")

    if FOLIAGE_PATTERNS.search(key):
        scores["foliage_alpha"] += 3.0
        reasons.append("foliage/cutout keyword match")

    if ROAD_PATTERNS.search(key):
        scores["road_surface"] += 3.0
        reasons.append("road/terrain keyword match")

    if BUILDING_PATTERNS.search(key):
        scores["buildings_props"] += 2.9
        reasons.append("building/prop keyword match")

    # Pixel-derived refinements
    if metrics is not None:
        if metrics.alpha_cutout:
            # Cutout alpha is a strong signal for foliage/fences/crowds unless
            # already clearly a car part (like car windows/livery decals).
            if scores["car_livery"] < 2.5:
                scores["foliage_alpha"] += 2.6
                reasons.append(f"alpha cutout detected ({metrics.transparent_ratio:.0%} transparent)")
            else:
                reasons.append("alpha channel on car texture")
        if metrics.looks_seamless:
            if scores["car_livery"] < 2.0 and scores["sky_backdrop"] < 2.0:
                scores["road_surface"] += 1.2
                reasons.append(
                    f"seamless borders (h={metrics.seam_h_diff:.1f}, v={metrics.seam_v_diff:.1f})"
                )
        if metrics.sky_gradient_score > 0.35 and scores["car_livery"] < 2.0:
            scores["sky_backdrop"] += 1.8 * metrics.sky_gradient_score
            reasons.append(f"sky gradient signal ({metrics.sky_gradient_score:.2f})")

    best_cat = max(scores, key=lambda c: scores[c])
    best_score = scores[best_cat]

    if best_score < 0.8:
        # Conservative fallback: seamless opaque tiles -> road_surface, else buildings_props
        if metrics is not None and metrics.alpha_cutout:
            best_cat = "foliage_alpha"
            reasons.append("fallback: alpha cutout")
            conf = 0.65
        elif metrics is not None and metrics.looks_seamless:
            best_cat = "road_surface"
            reasons.append("fallback: seamless tile")
            conf = 0.55
        else:
            best_cat = "buildings_props"
            reasons.append("fallback: default static world prop/building")
            conf = 0.45
    else:
        conf = min(0.97, 0.50 + best_score * 0.12)

    seamless = best_cat == "road_surface" or (
        best_cat == "buildings_props" and metrics is not None and metrics.looks_seamless
    )
    sub_profile = detect_sub_profile(key, path_lower, best_cat)
    if sub_profile:
        reasons.append(f"sub_profile={sub_profile}")
    return ClassificationResult(
        key=key,
        category=best_cat,
        confidence=round(conf, 3),
        seamless=seamless,
        has_alpha_cutout=has_cutout or best_cat == "foliage_alpha",
        reasons=tuple(reasons),
        sub_profile=sub_profile,
    )


def main(argv: Optional[Iterable[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Classify CMR2 textures (from a data dir, .BFL, or extracted PNG folder)."
    )
    parser.add_argument("input", help="Path to CMR2 data dir, .BFL file, or directory of PNGs")
    parser.add_argument("--inspect-pixels", action="store_true", help="Decode images to refine classification")
    parser.add_argument("--json", dest="json_out", help="Write classification manifest to JSON file")
    args = parser.parse_args(list(argv) if argv is not None else None)

    in_path = Path(args.input)
    results: List[ClassificationResult] = []

    if in_path.is_dir() and any(in_path.rglob("*.png")) and not any(in_path.rglob("*.bfl")):
        for png_path in sorted(in_path.rglob("*.png")):
            img = load_png(png_path) if args.inspect_pixels else None
            results.append(classify_texture(png_path.name, str(png_path.parent), image=img))
    else:
        sources, warnings = scan_data_directory(in_path)
        for w in warnings:
            print(f"[warn] {w}", file=sys.stderr)
        from bfl_extract import load_texture_source

        for src in sources:
            img = None
            if args.inspect_pixels:
                try:
                    img = load_texture_source(src)
                except Exception:
                    img = None
            results.append(classify_texture(src.original_name, src.relative_hint, image=img))

    counts = {c: 0 for c in CATEGORIES}
    for r in results:
        counts[r.category] += 1
        print(f"[{r.category:15s}] {r.key:24s} conf={r.confidence:.2f} seamless={r.seamless} ({'; '.join(r.reasons)})")

    print("\nSummary:")
    for c in CATEGORIES:
        print(f"  {c:16s}: {counts[c]}")

    if args.json_out:
        Path(args.json_out).write_text(
            json.dumps([r.to_dict() for r in results], indent=2), encoding="utf-8"
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

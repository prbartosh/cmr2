# OpenCMR2 Stage 10 — HD Texture Pipeline (`tools/hd_textures/`)

Samowystarczalny zestaw narzędzi Python (zero wymaganych zewnętrznych zależności w trybie bazowym; automatyczne przyspieszenie na Apple Silicon GPU/Neural Engine przez `PyTorch MPS`, `ONNX Runtime CoreML`, `Pillow`, `NumPy` i natywną bibliotekę C++ `libbc7enc.dylib`) do ekstrakcji, klasyfikacji, AI super-resolution (ESRGAN 4x), generowania neuralnych map normalnych (DeepBump256) oraz map PBR `_s`, a także kompresji natywnych tekstur **DX10 BC7 DDS z pełnym łańcuchem mipmap** dla **OpenCMR2**.

Self-contained Python toolkit (zero mandatory external dependencies in stdlib mode; automatically unlocks Apple Silicon GPU/CPU throughput via `PyTorch MPS`, `ONNX Runtime CoreML`, `Pillow`, `NumPy`, and native C++ `libbc7enc.dylib`) for read-only extraction, classification, AI 4x super-resolution, neural normal + PBR specular/roughness/clearcoat map generation, and native **DX10 BC7 DDS with pre-generated mipmaps** for **OpenCMR2**.

---

## Bezpieczeństwo plików gry / Read-Only Guarantee

Wszystkie skrypty działają **wyłącznie w trybie tylko do odczytu** na oryginalnych plikach gry (`.BFL`, `.DDS`, `.TGA`). Wszystkie wygenerowane zasoby trafiają wyłącznie do katalogu docelowego `--output` (`overrides/`).

All scripts open original CMR2 game archives and textures **read-only** and write exclusively to `--output` (`overrides/`).

---

## Nowości Stage 10 / Stage 10 Capabilities

### 1. Apple Silicon GPU AI Super-Resolution (`ai_upscaler.py`)
Wybór backendu przez `--upscaler {auto,mps_esrgan,ncnn,lanczos}`:
- **`mps_esrgan`**: Czysty PyTorch **RRDBNet (ESRGAN 4x, 23 bloki RRDB)** bez zależności od `basicsr` czy `spandrel`. Obsługuje zarówno klasyczny format wag ESRGAN (`4x-UltraSharp.pth`: `model.0.*`, `model.1.sub.0..22.RDB{1..3}...`) jak i BasicSR (`RealESRGAN_x4plus.pth`: `conv_first`, `body.*`). Uruchamiany na Apple Silicon **`mps` (fp16)** z automatycznym fallbackiem na CPU oraz kafelkowaniem z nakładaniem (overlap blending), dzięki czemu tekstury 2K/4K nigdy nie powodują OOM.
- **`ncnn`**: Opcjonalny wrapper CLI dla `realesrgan-ncnn-vulkan`.
- **`lanczos`**: Szybki backend zero-dependency (Lanczos-3 + Faithful+ micro-detail).
- **`auto`**: Automatycznie wybiera `mps_esrgan` (gdy dostępny jest PyTorch + model w `~/.cache/opencmr2_hd/models/4x-UltraSharp.pth` lub `--ai-model`), inaczej `ncnn`, inaczej `lanczos`.

Gwarancje jakości dla wszystkich backendów AI:
- **Periodyczne zawijanie krawędzi (`wrap_x` / `wrap_y`)**: przed inferencją AI tekstura jest poszerzana okresowo i dokładnie przycinana po skalowaniu — zero szwów na drogach i terenie.
- **Dylatacja RGB na kanałach alfa (`dilate_alpha_rgb`)**: AI skaluje wyłącznie dylatowany RGB, podczas gdy kanał Alpha jest skalowany czystym Lanczos-3 — zero ciemnych obwódek na drzewach i płotach.
- **Supersampling 2x**: dla docelowej skali 2x (np. 512→1024) model 4x generuje obraz 4x i wykonuje czysty downsample Lanczos-3.

### 2. Neuralne mapy normalnych i mapy PBR (`neural_maps.py`)
Wybór backendu przez `--normal-backend {auto,deepbump,hybrid,scharr}`:
- **`deepbump`**: Uruchamia `deepbump256.onnx` przez `onnxruntime` (`CoreMLExecutionProvider` → fallback `CPUExecutionProvider`) z cyklicznym kafelkowaniem modulo i płynnym oknem Hann — zero szwów kafelkowania, poprawiona orientacja osi Y zgodna z shaderem `fixed.frag` OpenCMR2.
- **`hybrid`** *(domyślny dla `auto` gdy obecny jest DeepBump256)*: łączy neuralną głębię powierzchni DeepBump256 (ignorującą namalowane pasy i naklejki sponsorów) z wysokoczęstotliwościowym mikro-detalem Scharr na drogach/kamieniu, jednocześnie tłumiąc fałszywe normalne krawędzi malowania (chroma-gradient suppression) na `car_livery`.
- **`scharr`**: Wieloskalowy gradient Scharr (zero zewnętrznych zależności).

### 3. Dedykowane sub-profile PBR dla aut, kokpitów i nawierzchni (`classify_textures.py`, `neural_maps.py`)
Zachowano pełną kompatybilność wsteczną 6 głównych kategorii (`CATEGORIES`), dodając precyzyjne pole `sub_profile`:
- `car_livery` → `car_body` (`*boddf`/`*dbodf`/`*bodf`), `car_cockpit` (`*int*`/`*cockpit*`/`*dash*`/`*seat*`/`*steer*`), `car_glass` (`*glid*`/`*glod*`/`*glob*`/`*windscr*`/`*glass*`), `car_wheel` (`*und*`/`*wh*`/`*tyr*`/`*tire*`/`*rim*`), `car_light` (`*lig*`/`*headl*`/`*taill*`).
- `road_surface` → `road_tarmac`, `road_gravel`, `road_snow`.
- Segmentacja pikseli w mapie `_s`:
  - `R`: Specular / Gloss (odwrotność roughness)
  - `G`: Odbicia Cubemap / Fresnel / metaliczność
  - `B`: Clearcoat (lakier karoserii, szyby, światła) lub mikro-AO szczelin (drogi, budynki; `0` dla roślinności)
  - `A`: `255`
- **Tiering rozdzielczości (`--hero-max-size`)**: pozwala skalować główne tekstury bohaterów (karoserie `car_body`, kokpity `car_cockpit`, drogi `road_surface`) do 2K/4K (np. `--hero-max-size 4096`), utrzymując mniejsze obiekty na limicie `--max-size` (np. `1024`/`2048`).

### 4. Natywny kompresor DX10 DDS / BC7 z pełnym łańcuchem mipmap (`bc7_dds.py`, `bc7_wrapper.cpp`)
Wybór formatu przez `--format {png,dds,bc7}` oraz jakości `--bc7-quality {ultra_fast,fast,medium,high}`:
- Zapisuje zgodne z loaderem OpenCMR2 pliki DX10 DDS (`DXGI_FORMAT_BC7_UNORM = 98`, `TEXTURE2D`) zawierające pełny łańcuch pre-generowanych mipmap aż do `1×1`:
  - **Albedo / alfa**: 2×2 box ważony kanałem alfa (identycznie z `texture_override.cpp` — brak ciemnienia liści na mipach).
  - **Normalne (`_n.dds`)**: uśrednianie wektorów stycznych + ponowna normalizacja na każdym poziomie mip.
  - **Specular/PBR (`_s.dds`)**: uśrednianie 2×2 z liniowymi wagami BC7.
- Automatyczny wybór najszybszego enkodera BC7:
  1. Natywna wielowątkowa biblioteka C++ `libbc7enc.dylib` / `.so` (`OPENCMR2_BC7_LIB`, `tools/hd_textures/libbc7enc.dylib`, lub `~/.cache/opencmr2_hd/bc7enc/libbc7enc.dylib`).
  2. Zewnętrzne narzędzia CLI w `PATH` (`bc7enc`, `texconv`, `compressonatorcli`, `cuttlefish`).
  3. Zero-dependency enkoder **BC7 Mode 6** (przyspieszany przez NumPy lub czysty Python stdlib).

Kompilacja `libbc7enc.dylib` ze źródła `bc7_wrapper.cpp` (macOS):
```bash
clang++ -O3 -std=c++17 -shared -fPIC \
  -I"$HOME/.cache/opencmr2_hd/bc7enc" \
  "$HOME/.cache/opencmr2_hd/bc7enc/bc7enc.cpp" \
  OpenCMR2/tools/hd_textures/bc7_wrapper.cpp \
  -o "$HOME/.cache/opencmr2_hd/bc7enc/libbc7enc.dylib"
```

---

## Konwencja nazewnictwa tekstur (`overrides/`) / Naming Convention

- OpenCMR2 pobiera nazwę pliku po ostatnim znaku `\` lub `/`, zamienia ją na **małe litery** i usuwa rozszerzenie `.dds` / `.tga`:
  - `\NEWIMAGE\OSD\CODRIV\ARROWS4.TGA` → `arrows4`
  - `Cars\Ford\FORDBODF.DDS` → `fordbodf`
- Dla formatu `--format png`: `<key>.png`, `<key>_n.png`, `<key>_s.png`.
- Dla formatu `--format bc7` / `--format dds`: `<key>.dds`, `<key>_n.dds`, `<key>_s.dds` (DX10 BC7 z pełnymi mipmapami).
- Tekstury wektorowe UI/fonty (`ui_skip`) są pomijane automatycznie; tekstury nieba (`sky_backdrop`) otrzymują wyłącznie teksturę albedo.

---

## Użycie / Usage

### 1. Pełny pipeline Stage 10 (Apple Silicon MPS + DeepBump256 + DX10 BC7 DDS)

```bash
# Pełny zestaw HD z AI ESRGAN 4x (MPS), neuralnymi normalnymi (Hybrid DeepBump256),
# 4K dla aut/dróg i natywną kompresją DX10 BC7 DDS z mipmapami:
~/.cache/opencmr2_hd/venv/bin/python OpenCMR2/tools/hd_textures/run_pipeline.py \
  --data "/path/to/Colin McRae Rally 2/data" \
  --output /path/to/OpenCMR2/overrides \
  --format bc7 \
  --upscaler auto \
  --normal-backend auto \
  --bc7-quality fast \
  --max-size 2048 \
  --hero-max-size 4096 \
  --workers 8

# Podgląd klasyfikacji i sub-profili bez zapisu plików (dry-run):
python3 OpenCMR2/tools/hd_textures/run_pipeline.py \
  --data /path/to/CMR2/data \
  --output /path/to/OpenCMR2/overrides \
  --dry-run \
  --manifest /tmp/cmr2_manifest.json

# Tryb zero-dependency (czysty Python stdlib, Lanczos-3 + Scharr + PNG):
/opt/homebrew/bin/python3 OpenCMR2/tools/hd_textures/run_pipeline.py \
  --data /path/to/CMR2/data \
  --output /path/to/OpenCMR2/overrides \
  --format png \
  --upscaler lanczos \
  --normal-backend scharr \
  --workers 8
```

### 2. Narzędzia składowe / Individual Tools

```bash
# Podgląd zawartości archiwum .BFL lub katalogu gry
python3 OpenCMR2/tools/hd_textures/bfl_extract.py /path/to/stage.bfl --list

# Ekstrakcja tekstur do PNG (read-only)
python3 OpenCMR2/tools/hd_textures/bfl_extract.py /path/to/stage.bfl -o /tmp/extracted

# Klasyfikacja tekstur (z podglądem sub-profili)
python3 OpenCMR2/tools/hd_textures/classify_textures.py /path/to/CMR2/data --inspect-pixels

# Pojedyncze skalowanie PNG -> BC7 DDS + _n.dds / _s.dds
python3 OpenCMR2/tools/hd_textures/upscale_hd.py /tmp/extracted/fordbodf.png \
  -o /tmp/overrides --format bc7 --upscaler auto --normal-backend auto
```

### 3. Testy jednostkowe / Unit Tests

```bash
# Zero-dependency pure-Python stdlib verification:
/opt/homebrew/bin/python3 OpenCMR2/tools/hd_textures/test_pipeline.py

# Accelerated Apple Silicon venv verification:
~/.cache/opencmr2_hd/venv/bin/python OpenCMR2/tools/hd_textures/test_pipeline.py
```

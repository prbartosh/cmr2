# Handoff — Odświeżenie wizualne Colin McRae Rally 2.0 (OpenCMR2 / macOS Metal)

> **Data aktualizacji:** 2026-11-26  
> **Stan bazowy (`main`):** Etapy 0–9 ukończone, zweryfikowane (24/24 `ctest`, 13/13 testów Python, smoke-test Metal na polskiej edycji + 11 939 plików tekstur HD w `~/Library/Application Support/OpenCMR2/overrides/`).  
> **Przeznaczenie:** Checkpoint i specyfikacja dla kolejnej sesji (Faza 2: Etapy 10–14 — „Max Visuals + High Performance”).

---

## 1. Środowisko i ścieżki

- **Repozytorium główne (`main`):** `/Users/bartosz/Desktop/cmr2` (zawiera pełny kod źródłowy `./OpenCMR2/` wraz z historią Etapów 0–9).
- **Oryginalne pliki gry (polska edycja, TYLKO DO ODCZYTU):**  
  `/Users/bartosz/Desktop/cmr2/Colin McRae Rally 2/Colin McRae Rally 2`
- **Katalog użytkownika OpenCMR2 (zapisy, `opencmr2.ini`, lokalne `overrides/`):**  
  `~/Library/Application Support/OpenCMR2/`
- **Zainstalowane narzędzia budowania (Homebrew, `/opt/homebrew/bin`):**  
  `cmake 4.5.1`, `ninja 1.13.2`, `glslang 16.6.0`, `spirv-cross 1.4.363.0`, `sdl3 3.4.20`, `python3 3.14`

### Komendy budowania i weryfikacji

```bash
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

# Konfiguracja (przy świeżym worktree bez OpenCMR2/build)
cd OpenCMR2 && cmake --preset macos

# Kompilacja + testy jednostkowe (w sandboxie — bez 2 testów UDP)
cd OpenCMR2 && cmake --build --preset macos && ctest --preset macos -E "net_session|net_frontend" --output-on-failure

# Testy sieciowe UDP (poza sandboxem)
/opt/homebrew/bin/ctest --test-dir OpenCMR2/build/macos -R "net_session|net_frontend" --output-on-failure

# Testy narzędzi tekstur HD (Python)
/opt/homebrew/bin/python3 OpenCMR2/tools/hd_textures/test_pipeline.py

# Smoke-test uruchomienia na polskiej edycji (poza sandboxem)
TMP_USER="/tmp/opencmr2_smoke_user"
rm -rf "$TMP_USER" && mkdir -p "$TMP_USER"
./OpenCMR2/build/macos/src/opencmr2 \
  --data "/Users/bartosz/Desktop/cmr2/Colin McRae Rally 2/Colin McRae Rally 2" \
  --user-dir "$TMP_USER" & PID=$!; sleep 3; kill -TERM $PID 2>/dev/null || true; wait $PID 2>/dev/null || true
cat "$TMP_USER/opencmr2.log"
```

---

## 2. Zrealizowana Faza 1 (Etapy 0–9 — już na `main`)

| Etap | Commit (OpenCMR2) | Zakres | Status |
| --- | --- | --- | --- |
| **Etap 0** | `4a5f430` | `visual_overhaul = 1`, `Sys_GetOptionFloat`, przełącznik live `F10`, preset `macos` | Zakończony |
| **Etap 1** | `6b2ea69` | Pełne mipmapy z ważeniem alfa dla DDS BC1/BC3 (`compressed_mipmaps = 1`), `anisotropy = 16`, trilinear 3D | Zakończony |
| **Etap 2** | `70ff30a` | Sprzętowe MSAA (`msaa = 4`, 2/4/8×) w `gfx_gpu.cpp` z `RESOLVE_AND_STORE` | Zakończony |
| **Etap 3** | `3c48e1a` | `draw_distance_scale = 1.4`, `distance_fade_start = 0.78`, `cubemap_size = 256`, `cubemap_object_distance = 60` | Zakończony |
| **Etap 4** | `3fcba28` | Per-pixel Blinn-Phong (8 świateł), hemisphere ambient, per-pixel Fresnel + clearcoat | Zakończony |
| **Etap 5** | `26379be` | Post-processing 3D-only: HDR `RGBA16F`, ACES Filmic Tone Mapping + 2-stopniowy Bloom (ostry HUD 2D) | Zakończony |
| **Etap 6** | `58d3ea6` | System `overrides/` (PNG/DDS/TGA + auto-mipmapy), `dump_textures = 1`, mapy `*_n.png` (screen-space TBN) i `*_s.png`, live `F10` | Zakończony |
| **Etap 7** | `21bbde0` | Real-time directional shadow mapping ze słońca (`D32_FLOAT` 2048×2048, texel-snapped ortho, alpha-test roślinności, 9-tap PCF) | Zakończony |
| **Etap 8** | `fb3730d` | Menu graficzne w grze (`Opcje → Grafika → Ekran → Grafika HD`, 6 języków w tym PL, zapis do `opencmr2.ini`) | Zakończony |
| **Etap 9** | `a56be01` | Narzędzia `tools/hd_textures/` (read-only `.BFL`, klasyfikacja, upscale 1K/2K + `*_n.png`/`*_s.png`) + wygenerowane 11 939 plików (3.6 GB) w `~/Library/Application Support/OpenCMR2/overrides/` | Zakończony |

---

## 3. Twarde gwarancje (obowiązują także w Fazie 2)

1. **Zero zmian w fizyce i modelu jazdy:** nie wolno modyfikować `CarPhysics.cpp`, `TrackCollision.cpp`, `X87Single.h`, `FTOI`, flag `-ffp-contract=off` ani tempa 25 Hz. Potwierdzane przez pełny `ctest` po każdym etapie.
2. **Zero modyfikacji oryginalnych plików gry:** `/Users/bartosz/Desktop/cmr2/Colin McRae Rally 2/` wyłącznie do odczytu.
3. **Pełna zgodność:** polska edycja, zapisy (`SavedGame.cpp`), pady (`src/input/`), dźwięk (`src/audio/`).
4. **Konfigurowalność i wydajność:** każda nowa funkcja ma klucz w `opencmr2.ini` (z bezpiecznym domyślnym ustawieniem pod płynne 60–120+ FPS na Apple Silicon), przełącznik w menu **Grafika HD** oraz natychmiastowe wyłączanie klawiszem **`F10`**.
5. **Atomowe commity:** jeden zweryfikowany commit na każdy etap (Etapy 10–14).

---

## 4. Plan Fazy 2 — „Max Visuals + High Performance” (Etapy 10–14)

1. **Etap 10 — Ekosystem tekstur nowej generacji: DDS BC7 (natywne GPU) + AI Upscaling (Real-ESRGAN / 4x-UltraSharp na Apple Silicon) + neuronowe mapy normalnych/roughness (DeepBump) + dedykowane presety aut i kokpitów**
   - Renderer (`texture_override.cpp`, `gfx_gpu.cpp`, `gfx_internal.h`): natywna obsługa kontenera **DDS DX10 / BC7** (`SDL_GPU_TEXTUREFORMAT_BC7_RGBA_UNORM`) z pre-generowanymi mipmapami wgrywanymi bezpośrednio na GPU (zero dekodowania PNG na CPU, ~4× mniej VRAM, 5–10× szybsze ładowanie odcinków, gotowość pod tekstury 2K/4K).
   - Pipeline (`tools/hd_textures/`): opcjonalny backend AI super-resolution na GPU Apple Silicon (Metal/NCNN lub PyTorch MPS z modelami `4x-UltraSharp` / `Real-ESRGAN`) + neuronowe normal/roughness mapy (DeepBump/ONNX z zachowaniem wrap-around tilingu i dylatacji alfa) + automatyczna kompresja wyjściowa do DDS BC7 z mipmapami + dedykowane maski PBR dla aut (`car_livery`) i kokpitów.
   - Regeneracja lokalnej paczki w `~/Library/Application Support/OpenCMR2/overrides/` w formacie BC7 (lub szybkim zoptymalizowanym formacie).

2. **Etap 11 — Kaskadowe mapy cieni (2-cascade CSM) + 16-tap Rotated Poisson PCF**
   - Rozbicie obecnej pojedynczej shadow mapy na 2 kaskady (bliska `0–20 m` ok. `1.5–2 cm/texel` wokół rajdówki + daleka `20–110 m`, obie texel-snapped) — np. w teksturze `TEXTURE_2D_ARRAY` (2 warstwy) lub atlasie 2×1 z minimalnym narzutem draw-calli (culling obiektów poza kaskadą).
   - 16-próbkowy obracany Poisson Disk PCF (obrót z screen-space IGN / blue noise) + płynne mieszanie na granicy kaskad.
   - Opcje: `shadow_cascades = 2`, `shadow_pcf_quality = 2`.

3. **Etap 12 — Screen-Space Ambient Occlusion (Half-Res GTAO + Bilateral Blur) + FidelityFX CAS**
   - Kopia/odczyt głębi 3D (`backDepth` -> `R32_FLOAT` lub `D32_FLOAT` z `SAMPLER`, z obsługą MSAA resolve głębi w shaderze lub oddzielnym lekkim passie) na **połówkowej rozdzielczości** dla maksymalnej wydajności.
   - Szybki **GTAO** (Ground-Truth Ambient Occlusion, 2 kierunki × 4 kroki na pół-rozdzielczości) + separowalny depth-aware bilateral blur, mnożony przed tone mappingiem w `post_composite.frag` (cienie kontaktowe pod kołami, karoserią, poboczem i drzewami).
   - **FidelityFX CAS** (Contrast Adaptive Sharpening) na końcu `post_composite.frag` (tylko 3D, przed HUD-em 2D).
   - Opcje: `ssao = 1`, `ssao_strength = 0.55`, `cas_sharpening = 1`, `cas_strength = 0.40`.

4. **Etap 13 — PBR (Cook-Torrance GGX + Prefiltered Cubemap IBL) + Mokre nawierzchnie w deszczu + Atmospheric/Height Fog**
   - Generowanie łańcucha rozmytych mipmap dla dynamicznej cubemapy odbić auta (szybki blur pass na 256×256) + przejście w `fixed.frag` z Blinn-Phonga na energooszczędny **GGX Specular + roughness/metalness/AO z `*_s.png`** (z zachowaniem pełnej wierności „Faithful+”).
   - Subtelna mokrość nawierzchni na odcinkach deszczowych (ciemniejszy mokry asfalt + mocniejsze odbicia Fresnela/cubemapy) oraz **Atmospheric / Height Fog** z delikatnym Sun In-Scattering (Mie glow) na horyzoncie.
   - Opcje: `pbr_shading = 1`, `wet_surfaces = 1`, `atmospheric_fog = 1`.

5. **Etap 14 — Alpha-to-Coverage (MSAA dla roślinności/siatek) + Soft Particles (kurz/śnieg/dym) + aktualizacja menu „Grafika HD” + profiling wydajności**
   - Włączenie `enable_alpha_to_coverage` dla geometrii z alpha-testem przy aktywnym MSAA (wyostrzanie krawędzi liści i siatek bez migotania).
   - **Soft Particles** (próbkowanie głębi sceny przy półprzezroczystych cząsteczkach kurzu/śniegu/dymu, usuwające twarde linie przecięcia z ziemią i nadwoziem).
   - Dodanie przełączników nowych funkcji do podmenu **Grafika HD** w `game/FrontendScreens.cpp` (6 języków, w tym PL).
   - Końcowa weryfikacja: pełny `ctest` (wszystkie testy + UDP), pomiar czasu klatki / FPS na Metal w wysokiej rozdzielczości, weryfikacja `F10` oraz opcjonalna aktualizacja paczki binarnej `OpenCMR2-macOS-arm64/`.

# OpenCMR2 Stage 9 — HD Texture Pipeline (`tools/hd_textures/`)

Samowystarczalny zestaw narzędzi Python (zero wymaganych zewnętrznych zależności; opcjonalne przyspieszenie przez `Pillow` + `NumPy`) do ekstrakcji, klasyfikacji oraz generowania tekstur HD („Faithful+” 1K/2K) wraz z mapami normalnych (`_n.png`) i odbić/połysku (`_s.png`) dla **OpenCMR2**.

Self-contained Python toolkit (zero mandatory external dependencies; automatically accelerates when `Pillow` + `NumPy` are installed) for read-only extraction, classification, and "Faithful+" 1K/2K HD enhancement + normal/specular map generation for **OpenCMR2**.

---

## Bezpieczeństwo plików gry / Read-Only Guarantee

Wszystkie skrypty działają **wyłącznie w trybie tylko do odczytu** na oryginalnych plikach gry (`.BFL`, `.DDS`, `.TGA`). Wszystkie wygenerowane zasoby trafiają wyłącznie do katalogu docelowego `--output` (`overrides/`).

All scripts open original CMR2 game archives and textures **read-only** and write exclusively to `--output` (`overrides/`).

---

## Zweryfikowany format archiwów `.BFL` / Verified CMR2 `.BFL` Format

Zweryfikowano bezpośrednio na podstawie kodu źródłowego OpenCMR2 (`game/BFL.h`, `game/FileBuffer.cpp`, `game/GenericFileLoader.cpp`):

1. **Warstwa zewnętrzna**: Plik `.bfl` otwierany jest przez `zlib` `gzopen(..., "rb")` — obsługuje zarówno archiwa skompresowane gzip (`1F 8B`), jak i nieskompresowane.
2. **Nagłówek `BFLHeader` (8 bajtów)**:
   - `char ident[4]`: stały znacznik `"CMPR"` (`43 4D 50 52`)
   - `uint32_t archiveSize` (little-endian): rozmiar właściwego bufora archiwum po nagłówku (`!= 0xFFFFFFFF`)
3. **Bufor archiwum (`archiveSize` bajtów)**:
   - Dane plików od offsetu `0`.
   - Ostatnie 4 bajty (`int32_t dirOffset`, little-endian): offset początku tabeli katalogu względem początku bufora (po nagłówku `CMPR`).
   - Rekordy katalogu od `dirOffset` do `archiveSize - 4`:
     - `uint32_t fileSize` (w `GenericFileLoader.cpp` pole `id`)
     - `int32_t fileOffset` (offset danych względem początku bufora)
     - `uint32_t fileNameLength` (długość nazwy ASCII bez znaku NUL)
     - `char fileName[...]` wyrównane w górę do wielokrotności **4 bajtów** (`(len + 3) & ~3`).

---

## Konwencja nazewnictwa tekstur (`overrides/`) / Naming Convention

Zweryfikowano na podstawie `game/Texture.cpp` (`Texture_GetPathStem`, `CTexture::FindLoadTexture`), `game/Graphics.cpp` (`LoadDDSTexture`, `LoadTGATexture`) oraz `src/render/vector_sprite.cpp` (`VectorSprite_Replace`):

- OpenCMR2 pobiera nazwę pliku po ostatnim znaku `\` lub `/`, zamienia ją na **małe litery** i usuwa rozszerzenie `.dds` / `.tga`:
  - `\NEWIMAGE\OSD\CODRIV\ARROWS4.TGA` → `arrows4`
  - `Cars\Ford\FORDBODF.DDS` → `fordbodf`
- Jeśli w archiwum istnieją jednocześnie `.dds` i `.tga` o tym samym kluczu, preferowane jest `.dds` (zgodnie z `CTexture::FindLoadTexture`).
- Dla każdej przetworzonej tekstury pipeline zapisuje w `overrides/`:
  - `<key>.png` — ulepszona tekstura albedo/diffuse (RGBA8, zachowany stosunek boków i UV 1:1)
  - `<key>_n.png` — mapa normalnych w przestrzeni stycznej (neutralny płaski normal `(128, 128, 255)`)
  - `<key>_s.png` — maska połysku i odbić:
    - `R`: siła specular (`0..255`)
    - `G`: mnożnik odbić cubemap / Fresnel (`0..255`)
    - `B`: warstwa clearcoat / lakier (`0..255`)
    - `A`: `255`

---

## Kategorie tekstur / Texture Categories

| Kategoria | Przykłady | Przetwarzanie | Mapy `_n` / `_s` |
| --- | --- | --- | --- |
| `road_surface` | asfalt, szuter, śnieg, błoto, piasek, trawa | Periodyczne zawijanie krawędzi (zero szwów kafelkowania) + subtelne mikro-ziarno | Tak (periodyczne normalne) |
| `car_livery` | karoserie (`*bodf`), malowania, koła, wnętrze, szyby | Czyste skalowanie 1:1 UV bez szumu, ostre napisy sponsorów | Tak (subtelne normalne, wysoki połysk/clearcoat) |
| `foliage_alpha` | drzewa, krzaki, płoty, publiczność, znaki z kanałem alfa | Dylatacja RGB (color bleeding) przed skalowaniem — brak czarnych/białych obwódek | Tak (wygaszane na pikselach przezroczystych) |
| `buildings_props` | budynki, mury, bariery, banery, skały | Wyostrzenie detali krawędziowych + lekka faktura | Tak (mocniejsze normalne, matowe odbicia) |
| `sky_backdrop` | niebo, chmury, horyzont | Płynne skalowanie (zawijanie poziome) | **Nie** (tylko albedo) |
| `ui_skip` | fonty (`hel_*`, `lcd_*`), wektorowe ikony/mapy OpenCMR2 (`ar_640*`, `smmatrix`, `arrows4`, `cha01..08`, `tick`, `cross`, `box`, `circle`, `chalblob`, `dot*`, `*digit`, `*revct`) | **Pomijane automatycznie** (nietknięte) | **Nie** |

---

## Użycie / Usage

### 1. Pełny pipeline (`run_pipeline.py`)

```bash
# Podgląd klasyfikacji bez zapisu plików (dry-run)
python3 OpenCMR2/tools/hd_textures/run_pipeline.py \
  --data /path/to/CMR2/data \
  --output /path/to/OpenCMR2/overrides \
  --dry-run \
  --manifest /tmp/cmr2_manifest.json

# Generowanie paczki HD (wszystkie kategorie 3D, wielowątkowo)
python3 OpenCMR2/tools/hd_textures/run_pipeline.py \
  --data /path/to/CMR2/data \
  --output /path/to/OpenCMR2/overrides \
  --workers 8

# Szybki test na wybranych kategoriach i limicie 25 tekstur
python3 OpenCMR2/tools/hd_textures/run_pipeline.py \
  --data /path/to/CMR2/data \
  --output /path/to/OpenCMR2/overrides \
  --categories road_surface,car_livery \
  --limit 25 \
  --workers 4
```

### 2. Narzędzia składowe / Individual Tools

```bash
# Podgląd zawartości archiwum .BFL lub katalogu gry
python3 OpenCMR2/tools/hd_textures/bfl_extract.py /path/to/stage.bfl --list

# Ekstrakcja tekstur do PNG (read-only)
python3 OpenCMR2/tools/hd_textures/bfl_extract.py /path/to/stage.bfl -o /tmp/extracted

# Klasyfikacja tekstur
python3 OpenCMR2/tools/hd_textures/classify_textures.py /path/to/CMR2/data --inspect-pixels

# Pojedyncze skalowanie PNG + generacja _n.png / _s.png
python3 OpenCMR2/tools/hd_textures/upscale_hd.py /tmp/extracted/uk_tarmac01.png -o /tmp/overrides
```

### 3. Opcjonalne przyspieszenie / Optional Acceleration

Narzędzia działają out-of-the-box na czystej bibliotece standardowej Pythona 3. Przy przetwarzaniu całej gry (setki tekstur 2K) zaleca się doinstalowanie `Pillow` i `NumPy`, które zostaną użyte automatycznie:

```bash
pip install pillow numpy
```

### 4. Testy jednostkowe / Unit Tests

```bash
python3 OpenCMR2/tools/hd_textures/test_pipeline.py
```

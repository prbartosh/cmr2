# System nadpisywania tekstur (`overrides/`) i map materiałowych (`*_n.png`, `*_s.png`)

OpenCMR2 pozwala bezinwazyjnie zastępować dowolne tekstury z oryginalnych archiwów `.BFL` plikami w wyższej rozdzielczości (1K/2K/4K) bez modyfikowania oryginalnych plików gry.

## Kolejność wyszukiwania katalogów

1. Ścieżka ustawiona w `opencmr2.ini` w sekcji `[paths] overrides = ...`
2. `<katalog_użytkownika>/overrides/`
3. `./overrides/` (katalog roboczy)

## Obsługiwane formaty i nazewnictwo

- Obsługiwane formaty: `.png` (najwyższy priorytet), `.dds` (DXT1/DXT5/32-bit), `.tga`.
- Dopasowanie nie rozróżnia wielkości liter.
- Nazwy plików mogą używać samej znormalizowanej nazwy bez rozszerzenia (np. `fordbodf.png`) lub pełnej ścieżki względnej (np. `cars/ford/fordbodf.png`).

## Opcjonalne mapy pomocnicze (Etap 6)

Obok dowolnej tekstury `<nazwa>.png` (lub nawet samodzielnie) można umieścić:

- `<nazwa>_n.png` — mapa normalnych w przestrzeni stycznej (neutralny wektor `(128, 128, 255)`), obliczana w `fixed.frag` metodą screen-space cotangent frame (TBN z `dFdx`/`dFdy`).
- `<nazwa>_s.png` — maska połysku i odbić:
  - kanał **R**: mnożnik połysku specular / clearcoat,
  - kanał **G**: mnożnik odbić cubemapy i krzywej Fresnela.

## Eksport tekstur gry (`dump_textures = 1`)

Ustawienie w `opencmr2.ini`:

```ini
[video]
dump_textures = 1
```

zapisuje każdą wczytaną teksturę gry jako PNG do `<katalog_użytkownika>/dumped_textures/` z gotowymi nazwami do przeróbek HD.

## Przełącznik `F10`

Klawisz `F10` w locie przełącza tekstury HD oraz mapy `*_n.png` / `*_s.png` między stanem odświeżonym a oryginalnymi teksturami z `.BFL`.

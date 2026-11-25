# Colin McRae Rally 2.0 (OpenCMR2 — macOS)

Lokalna konfiguracja do uruchamiania **Colin McRae Rally 2.0** natywnie na macOS (Apple Silicon / Metal) przy użyciu silnika [OpenCMR2](https://github.com/pablocpas/OpenCMR2).

## Wymagana struktura katalogów

Pliki gry oraz binarki silnika są ignorowane przez Git (`.gitignore`). Aby uruchomić grę, w katalogu głównym powinny znajdować się:

- `OpenCMR2-macOS-arm64/` — paczka OpenCMR2 dla macOS (`arm64`) z `opencmr2` i `libSDL3.0.dylib`
- `Colin McRae Rally 2/Colin McRae Rally 2/` — oryginalna instalacja PC gry (`GAME/`, `FRONTEND/`, `SOUNDS/`, `CountrySpecific/` itd.)

## Uruchamianie

- **Z Findera:** podwójne kliknięcie na `Uruchom Colin McRae Rally 2.command`
- **Jako aplikacja macOS:** `/Applications/Colin McRae Rally 2.app` (Spotlight / Launchpad)
- **Z Terminala:**
  ```bash
  ./OpenCMR2-macOS-arm64/opencmr2 --data "./Colin McRae Rally 2/Colin McRae Rally 2"
  ```

> Przy pierwszym uruchomieniu nowo pobranej paczki OpenCMR2 należy zdjąć kwarantannę macOS:
> ```bash
> xattr -dr com.apple.quarantine OpenCMR2-macOS-arm64
> ```

## Zapisy i konfiguracja

Ustawienia gry, grafiki (`opencmr2.ini`), sterowania (`controls.ini`) oraz zapisy przechowywane są w:

```text
~/Library/Application Support/OpenCMR2/
```

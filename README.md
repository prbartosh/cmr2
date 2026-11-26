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

## Instalacja na innym Macu (z Dysku Google)

Pliki gry i binarki silnika nie są w repozytorium (zasoby gry są chronione prawem autorskim). Właściciel trzyma je w prywatnym folderze na swoim Dysku Google:

- `Colin McRae Rally 2/` (oryginalne pliki gry)
- `OpenCMR2-macOS-arm64/` (silnik: `opencmr2`, `libSDL3.0.dylib`, serwer `opencmr2-lobby`, licencje)
- `Uruchom Colin McRae Rally 2.command` (launcher)
- `INSTRUKCJA.txt` (skrócona instrukcja)
- `cmr2-tekstury-HD.zip` (opcjonalne tekstury HD, ok. 3,8 GB, po rozpakowaniu ok. 6,2 GB)

### Wymagania

Mac z procesorem Apple Silicon (M1 lub nowszy), macOS 11 Big Sur lub nowszy. SDL3 jest w paczce, Homebrew nie jest potrzebny.

### Pobieranie

- **Dysk Google w przeglądarce:** prawy klik na folderze, **Pobierz**. Dysk pakuje folder w jeden lub kilka plików zip: rozpakuj wszystkie części do jednego folderu (np. `~/Gry/cmr2`).
- **Aplikacja Dysk Google na Macu:** oznacz folder jako dostępny offline albo skopiuj go z folderu Dysku w inne miejsce (np. `~/Gry/cmr2`). Uruchamianie gry wprost ze strumieniowanego folderu Dysku jest wolne.

### Pierwsze uruchomienie

W Terminalu, w pobranym folderze:

```bash
xattr -dr com.apple.quarantine .
chmod +x "Uruchom Colin McRae Rally 2.command" OpenCMR2-macOS-arm64/opencmr2 OpenCMR2-macOS-arm64/opencmr2-lobby
```

Potem podwójne kliknięcie na `Uruchom Colin McRae Rally 2.command`.

### Tekstury HD (opcjonalnie)

```bash
mkdir -p "$HOME/Library/Application Support/OpenCMR2"
ditto -x -k cmr2-tekstury-HD.zip "$HOME/Library/Application Support/OpenCMR2/"
```

Tekstury trafiają do `~/Library/Application Support/OpenCMR2/overrides`. Gra przez internet z kodem wymaga jeszcze ustawienia `lobby` (sekcja poniżej).

### Własna paczka z tego repozytorium

Kto ma własną kopię gry, może zbudować silnik sam, zamiast pobierać paczkę: [OpenCMR2/README.md](OpenCMR2/README.md), sekcja *Building* (preset `macos`). Żeby paczka działała na innym Macu bez Homebrew, zbuduj ją z SDL3 kompilowanym ze źródeł:

```bash
cd OpenCMR2
cmake --preset macos -B build/macos-package -DOPENCMR2_VENDORED_SDL=ON -DOPENCMR2_BUILD_TESTS=OFF
cmake --build build/macos-package --target opencmr2 opencmr2-lobby
```

Do `OpenCMR2-macOS-arm64/` trafiają `build/macos-package/src/opencmr2`, `build/macos-package/src/lobby/opencmr2-lobby` i `build/macos-package/_deps/sdl3-build/libSDL3.0.dylib`. W `opencmr2` usuń rpath do katalogu builda (`install_name_tool -delete_rpath`, zostaje tylko `@executable_path`) i podpisz pliki ponownie (`codesign --force -s -`).

## Gra przez internet (kod gry)

Multiplayer przez internet bez przekierowywania portów: gospodarz dostaje krótki kod (np. `7KQ2-M9XA`), znajomy wpisuje go w grze. Połączenie ustawia mały serwer pośredniczący (lobby), a sama gra toczy się bezpośrednio między graczami (albo przez relay lobby, gdy routery nie przepuszczają ruchu).

### Co jest potrzebne

- Obaj gracze uruchamiają tę wersję silnika (zbudowaną z tego repozytorium i brancha). Wydanie upstream nie ma kodów gry.
- Obaj mają ustawiony ten sam serwer lobby. Albo w pliku `~/Library/Application Support/OpenCMR2/opencmr2.ini`:
  ```ini
  [net]
  lobby = adres[:port]
  ```
  (port domyślny to 28630), albo w buildzie z `-DOPENCMR2_LOBBY=adres` przy konfiguracji CMake. Wpis w `opencmr2.ini` ma pierwszeństwo.
- Działające lobby, osiągalne z internetu (UDP 28630 oraz 28631-28694 dla relay). Uruchomienie serwera (binarka, Docker, systemd): [OpenCMR2/docs/LOBBY.md](OpenCMR2/docs/LOBBY.md). Bez ustawionego lobby kody nie działają, a linia z kodem u gospodarza się nie pojawia.
- Opcjonalnie `relay = auto|always|never` w `[net]` (domyślnie `auto`): `auto` próbuje najpierw połączenia bezpośredniego (ok. 3 s), potem przez relay lobby; `always` zawsze przez relay; `never` tylko bezpośrednio. Relay nie szyfruje ruchu, więc operator lobby może go widzieć.

### Gospodarz

1. **Sieć → Sesja → Gospodarz**, ustaw grę (nazwa, tryb, liczba graczy, opcjonalnie hasło) i zatwierdź.
2. W lobby gry pojawia się linia **Kod gry: XXXX-XXXX**. Kod jest automatycznie kopiowany do schowka (obok kodu widać „(skopiowano do schowka)”).
3. Wyślij kod znajomemu. Kod działa, dopóki gra jest otwarta i połączona z lobby.

### Gość

1. **Sieć → Sesja → Dołącz kodem**.
2. Wpisz kod z klawiatury albo wklej ze schowka (**Ctrl+V** lub **Cmd+V**). Wielkość liter i myślnik nie mają znaczenia (litery I i L są czytane jak 1, O jak 0).
3. **Enter**. Gra szuka sesji i dołącza jak do zwykłej gry.

### Komunikaty i co robić

| Komunikat | Znaczenie |
| --- | --- |
| Szukanie gry... | pytanie do lobby o kod |
| Łączenie bezpośrednie... | próba połączenia z gospodarzem |
| Łączenie przez serwer pośredniczący... | połączenie przez relay lobby (zwykle przy trudnych routerach, np. CGNAT) |
| Nieprawidłowy kod | literówka lub zły format: sprawdź kod (8 znaków) |
| Nie znaleziono gry o tym kodzie | kod nieznany lub gra już zamknięta: poproś o aktualny kod |
| Za dużo prób, odczekaj chwilę | limit zapytań do lobby: odczekaj kilka sekund |
| Brak połączenia z serwerem kodów | lobby nieosiągalne: sprawdź `lobby` w `opencmr2.ini` i czy serwer działa |
| Nie można połączyć się z gospodarzem | brak ruchu między graczami (przy `relay = never` wypróbuj `auto`) |
| Gra jest pełna lub niedostępna | sesja jest pełna albo nie da się do niej dołączyć |
| Brak serwera kodów: ustaw [net] lobby w opencmr2.ini | nie ustawiono `lobby` |

U gospodarza zamiast kodu mogą być widoczne: „Kod gry: rejestrowanie...” (czeka na lobby) lub „Kod gry: serwer kodów nieosiągalny” (sprawdź ustawienie `lobby` i sieć).

Graj tylko ze znajomymi: kod pozwala poprosić o dołączenie, ale logika gry ufa temu, co dostaje od innych graczy. Szczegóły: [OpenCMR2/docs/NETWORK.md](OpenCMR2/docs/NETWORK.md).

## Zapisy i konfiguracja

Ustawienia gry, grafiki (`opencmr2.ini`), sterowania (`controls.ini`) oraz zapisy przechowywane są w:

```text
~/Library/Application Support/OpenCMR2/
```

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

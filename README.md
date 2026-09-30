# KSNetFix — poprawka multiplayer dla KnightShift / Polanie II

Nieoficjalna poprawka gry sieciowej do **KnightShift (Polanie II)** w wersji Steam 1.3.
Naprawia opóźnienia i synchronizację, klawiaturę, a gra przez Internet działa przez Steam —
bez Radmina, VPN-ów i przekierowywania portów.

*English summary [below](#english).*

**Pobierz:** [Releases](../../releases) → `KSNetFix-<wersja>.zip`, rozpakuj, uruchom `ZAINSTALUJ.bat`.

## Co daje

- **Brak lagu u dołączonego gracza** — reaguje na kliknięcia tak szybko jak host
  (w oryginale ok. 1 s opóźnienia rozkazów).
- **Równe tempo gry** — dokładny zegar zamiast źle kalibrowanego zegara procesora (naprawia też
  złe tempo na CPU powyżej 4,29 GHz), timer 1 ms, nadrabianie po zacięciach zamiast trwałego lagu.
- **Mniej „Waiting for player…”** przy wahaniach pingu.
- **Wykrywanie rozsynchronizowania** — każdy pakiet niesie skrót stanu gry; rozjazd jest zapisywany
  w logu (`DESYNC DETECTED`). Render nie zużywa już liczb losowych symulacji (źródło desyncu zależne od FPS).
- **Klawiatura** — poprawne wpisywanie tekstu (wcześniej gubione/losowe litery, trzeba było wciskać
  dwa klawisze naraz), z polskimi znakami (AltGr).
- **Gra przez Steam** — opcja „TCP/IP” działa przez lobby Steam i Steam Datagram Relay (serwery
  pośredniczące Valve). Sesje widać na liście z pingiem, można zapraszać znajomych.

Wszystko jest zgodne z protokołem gry (długość tury bez zmian), ale **każdy gracz powinien mieć
tę samą wersję** — przez Steam widzą się tylko gracze z KSNetFix.

## Instalacja

Wymagania: KnightShift ze Steama, Windows 10/11, włączony składnik Windows **DirectPlay**
(Panel sterowania → Programy → Włącz lub wyłącz funkcje systemu Windows → Starsze składniki),
uruchomiony klient Steam, gra na koncie Steam każdego gracza.

1. Zamknij grę, rozpakuj ZIP, uruchom `ZAINSTALUJ.bat` (sam znajdzie folder gry).
   Ręcznie: skopiuj `dinput8.dll`, `steam_api.dll` i `ksnetfix.ini` obok `KnightShift.exe`.
2. Uruchom grę normalnie. W folderze gry pojawi się `ksnetfix.log` — pierwsze linie kończą się `ok`.

Odinstalowanie: `ODINSTALUJ.bat` albo usuń te trzy pliki. Poprawka nie zmienia plików gry —
łata kod w pamięci po sprawdzeniu sygnatur; nieznany plik wykonywalny = zwykły `dinput8.dll`.
Szczegóły dla graczy: [`package/INSTRUKCJA.txt`](package/INSTRUKCJA.txt).

## Gra

- **Przez Steam (domyślnie):** host: Multiplayer → TCP/IP → nazwa sesji → Utwórz nową sesję;
  pozostali: Multiplayer → TCP/IP → sesja na liście „Dostępne sesje” → Dołącz. W polu adresu można
  też wpisać SteamID64 hosta albo numer lobby. `[Steam] AppId` musi być taki sam u wszystkich
  (domyślnie 254060 = KnightShift).
- **LAN / Radmin / IP (bez Steama):** `[Steam] Enabled=0` u wszystkich — TCP/IP działa jak w oryginale,
  pozostałe poprawki nadal działają.

Wszystkie opcje są opisane w [`ksnetfix.ini`](ksnetfix.ini).

## SteamNet — następca EarthNet (w budowie)

Pozycja „EarthNet” na liście połączeń nazywa się teraz „SteamNet” i działa: gra łączy się z małym
serwerem wbudowanym w KSNetFix zamiast z martwym `netserver.earthnet.de` (na liście serwerów pojawia się
„SteamNet — steam”, pozycje dodane przez gracza zostają).

- Na razie tryb offline: logowanie (dowolny login), kanał z czatem, szepty, bohaterowie RPG zapisywani
  w `SteamNet\` w folderze gry. Inni gracze, kanały i gry przez lobby Steam — w kolejnych wersjach.
- Ustawienia: sekcja `[SteamNet]` w `ksnetfix.ini`; `Trace=1` zapisuje w logu każdą linię wymienianą z grą.
- Protokół klienta EarthNet (logowanie, podpis, komendy): [`docs/STEAMNET_PROTOCOL.md`](docs/STEAMNET_PROTOCOL.md).

## Stan testów

Wersja wczesna, rozwijana na podstawie analizy (reverse engineering) protokołu gry.

| co | stan |
|---|---|
| silnik „D3D8 classic” (`KnightShift.ex1`) | sprawdzony w grze (wszystkie łatki `ok`) |
| silnik „shaders” (`KnightShift.ex2`) | adresy przeniesione i weryfikowane sygnaturami, **w grze jeszcze nieuruchamiany** |
| gra LAN, 2 instancje na jednym PC (KSNetFix 2.0) | ok. 4 min gry u klienta: 0 rozjazdów w 1565 porównaniach stanu, kolejka tur u dołączonego gracza średnio 0,8 (bez nadrabiania 2,2) |
| transport Steam poza grą (`steamtest host/enum`), lobby Steam w grze | sprawdzone |
| Steam Datagram Relay dla App ID 254060 (`sdrtest`) | sprawdzone: tylko przez przekaźnik, RTT ~19 ms |
| pełna gra 2 graczy przez Internet / Steam | **jeszcze niesprawdzona** — zgłaszaj wyniki z logami |
| SteamNet: protokół i hooki winsock (`test/earthnet_test.cpp`, `steamnet_test.exe` w CI na Windows) | sprawdzone poza grą; **w grze jeszcze nieuruchamiany** |

Problemy zgłaszaj w [Issues](../../issues) z plikami `ksnetfix.log` od hosta i gracza
(przy problemach ze Steamem: `[Steam] Verbose=1`).

## Jak to działa

Opis protokołu (lockstep, tury, pakiety, zegar), znalezionych błędów i poprawek:
[`docs/NETCODE.md`](docs/NETCODE.md). W skrócie:

- `dinput8.dll` to proxy ładowane przez grę; przy starcie rozpoznaje build po sygnaturach i łata kod
  w pamięci, potem przekazuje DirectInput do systemowego `dinput8.dll`.
- **Lockstep:** każdy komputer liczy całą symulację, host wyznacza tury (5 ticków po 33 ms). KSNetFix
  zmienia tylko *kiedy* wykonują się ticki (nadrabianie, dokładny zegar), nigdy ich liczbę na turę.
- **Transport Steam:** własna implementacja `IDirectPlay8Peer` (clean-room) podstawiana przez hook
  `CoCreateInstance` — sesje to lobby Steam, ruch idzie przez Steam Networking Messages.

## Budowanie

Windows, Git Bash, Visual Studio 2022 (Build Tools wystarczą, komponent C++ x86) i Windows SDK.
Potrzebny jest Steamworks SDK (nagłówki + `redistributable_bin`): pobierz ze
[strony Valve](https://partner.steamgames.com/downloads/list) i rozpakuj folder `sdk` do katalogu
repozytorium albo ustaw `STEAMWORKS_SDK` na folder z `public/steam/steam_api.h`.

```bash
./build.sh           # build/dinput8.dll + steam_api.dll
./build.sh tests     # build/test: kbtest, steamtest (host|enum), sdrtest (status|server|client), steamnet_test
./package.sh         # dist/KSNetFix-<wersja>.zip
```

Wydanie: podbij `KSNETFIX_VERSION` w `src/ksnetfix.cpp`, dodaj `docs/release-notes/v<wersja>.md` i wypchnij
tag `v<wersja>` — workflow `.github/workflows/release.yml` zbuduje paczkę MSVC na Windows
(Steamworks SDK z crate'a `steamworks-sys`) i opublikuje release z zipem.

## Serwer dedykowany

Serwer dedykowany RPG (automatyczny host, także VPS) jest rozwijany jako dodatek do KSNetFix w osobnym
repozytorium: [KnightShift-DedicatedServer](https://github.com/Joe13PL/KnightShift-DedicatedServer).

## Licencja i zastrzeżenia

Kod na licencji [MIT](LICENSE). `steam_api.dll` w paczkach to redystrybuowalna biblioteka Valve
(Steamworks SDK). Projekt nie jest związany z twórcami ani wydawcami gry; KnightShift / Polanie II
to znaki towarowe ich właścicieli. Repozytorium nie zawiera plików gry — potrzebna jest własna kopia.

---

## English

**KSNetFix** is an unofficial multiplayer fix for **KnightShift (Polanie II)**, Steam version 1.3.

- Removes the ~1 s command lag of joined players (the client catches up on queued turns instead of
  lagging forever), replaces the badly calibrated RDTSC clock (also broken above 4.29 GHz), uses a
  1 ms timer, tolerates ping jitter better.
- Detects desyncs (simulation hash in every packet) and keeps rendering from consuming simulation RNG.
- Fixes keyboard text input (lost / random letters).
- Online play over Steam: the game's "TCP/IP" option runs over Steam lobbies and the Steam Datagram
  Relay — no VPN, no port forwarding.
- SteamNet (work in progress): the dead EarthNet lobby entry becomes "SteamNet" and connects to a small
  server inside KSNetFix (login, channel chat, RPG heroes); Steam lobbies for other players come next.

Install: download the ZIP from [Releases](../../releases), run `ZAINSTALUJ.bat` (or copy `dinput8.dll`,
`steam_api.dll`, `ksnetfix.ini` next to `KnightShift.exe`). Requires Windows 10/11 with the DirectPlay
feature enabled, the Steam client running and the game on every player's Steam account. Every player
needs the same KSNetFix version. The game files are not modified; patches are applied in memory after
signature checks. Protocol notes: [`docs/NETCODE.md`](docs/NETCODE.md) (Polish). MIT licensed; not
affiliated with the game's developers or publishers.

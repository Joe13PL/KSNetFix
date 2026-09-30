# Protokół EarthNet (klient w KnightShift.ex1) — na potrzeby SteamNet

Stan: 2026-09-30, z pliku `KnightShift_ex1.exe` (Steam 1.3). Implementacja: `src/earthnet_core.cpp`
(protokół), `src/earthnet.cpp` (Windows), testy w `src/test/earthnet_test.cpp` i `src/test/steamnet_wine_test.cpp`. Opis tego, co **klient gry** wysyła i czego
oczekuje od serwera. Zweryfikowane w kodzie maszynowym, jeszcze nie na żywej grze.

## Połączenie

* Klient EarthNet: obiekt `g_enClient` `0xF625B0`, vtable `0x8FABA0` (spis metod niżej).
* Winsock z tabeli `{char* nazwa, void** wskaźnik}` od `0x922A28` do `0x922BA8` (koniec = NULL), wypełnianej
  przy starcie przez `0x6FD3B0("ws2_32.dll")`. Wskaźniki: `connect` `0xDA8574`, `socket` `0xDA8528`,
  `recv` `0xDA8544`, `send` `0xDA8538`, `WSAAsyncSelect` `0xDA84CC`, `WSAAsyncGetHostByName` `0xDA84D8`.
* Adres: lista serwerów `HKCU\Software\Reality Pump\KnightShift\BaseGame\Network\EarthNet`,
  `AddressIP` = `"nazwa""adres""nazwa""adres"…`, port z `Port` (REG_DWORD, domyślnie 17171).
* Tryb połączenia `+0x4A88` jest zawsze 0 (konstruktor `0x81F1D0` → `0x7FCED0(0)`), czyli
  **tryb binarny + linie ANSI**. Tryb 1 (same linie UTF-16, port +10) jest martwy.
* Kodowanie tekstu: strona kodowa ANSI systemu (`MultiByteToWideChar(CP_ACP)` / `WideCharToMultiByte(CP_ACP)`).

## Etap 1 — pakiety binarne

Każdy pakiet: `u32 długość_całkowita` (łącznie z tym polem) + **strumień zlib** z treścią (klasa strumienia gry
`0x7995D0`: flagi `0x6001` = odczyt przez inflate, `0x6002` = zapis przez deflate poziom 6; zlib 1.1.3).
Opis pól niżej dotyczy treści po rozpakowaniu. Liczby little-endian, napis = `u32 n` + `n`
bajtów ANSI bez zera.

| kierunek | stan klienta | treść |
|---|---|---|
| K→S | po `connect` (stan 1) | „client information”: blob przygotowany na ekranie wyboru serwera (`0x824040` → vfunc `+0x04`), treść nieistotna dla serwera — do zalogowania |
| S→K | stan 4 → `0x802F30` | `i32 kod`, `napis komunikat`. Kod 0 = dalej, inny = błąd (komunikat w oknie) |
| K→S | stan 5 (`0x803110`) | `napis login`, `napis hasło`, `u32 0`, `u32 n`, `n × GUID(16)` (lista `+0x5080`), `u32 reconnect`, [jeśli reconnect: `napis`, `napis`, `GUID`, `u32`, `napis`, `u32`], `GUID(16)` (`+0x506C`) |
| S→K | stan 7 → `0x803820` | odpowiedź na logowanie (niżej); potem stan 0 = linie tekstu |

### Logowanie kontem Steam

Metoda łączenia klienta `0x7FF8D0(host, port, login, hasło)` zapisuje login jako napis Unicode pod `+0x4A80`
(`{licznik 1, pojemność, długość, znaki, 0}`, pamięć z `Mem_Alloc 0x7971D0`), a dopiero potem woła
`WSAAsyncGetHostByName` (`0x7FFC50`). Pakiet logowania (`0x803110`) bierze nick z `+0x4A80` (Unicode → ANSI,
długość z nagłówka napisu) synchronicznie, gdy dostanie powitanie serwera (`0x8023C0`, stan 4). SteamNet podmienia
ten napis na nazwę konta Steam (bez `"` i `%`, najwyżej 16 bajtów) na wątku serwera **tuż przed wysłaniem
powitania**; sam napis powstaje wcześniej, w naszym `connect()` na wątku gry (alokator gry). Okno logowania się nie
pokazuje, gra zapisuje login w profilu gracza. Sprawdzone emulacją `0x7FF8D0`.
**Który obiekt:** łączy się nie zawsze obiekt spod `0xF625B0` — obsługa wyniku logowania (`0x81F9C0`) robi
`DAT_00f625b0 = this` dopiero po sukcesie („najszybszy serwer” łączy się własnym obiektem). SteamNet bierze obiekt
z bufora, który `0x7FF8D0` podaje do `WSAAsyncGetHostByName` (`this + 0x4AB0`, sprawdzone emulacją), i sprawdza
vtable `0x8FABA0`. 2.5.6–2.5.8 zmieniały login w obiekcie globalnym, więc gra i tak logowała się starym loginem.

**Siebie na liście graczy** gra rozpoznaje po id `+0x4EEC` i GUID `+0x506C` (`0x8207D0`): wpis z innymi jest
rysowany kolorem „inny gracz” (`translateEarthNetNewUserFormat`, żółty). GUID to ostatnie 16 bajtów pakietu
logowania (tekst GUID-u w `$user` = bajty w tej samej kolejności, sprawdzone emulacją). Serwer **nie wysyła**
`$user` gracza: klient dodaje się sam po zalogowaniu (`0x81F9C0`) i przy każdym wejściu na kanał (`0x823540`,
po `/join`), więc własny wpis dawał duplikat.

**Zapamiętany login** `DAT_00a58f08` (napis ANSI, ten sam nagłówek) gra podaje do `0x7FF8D0`, zapisuje w profilu
i pokazuje w „X wszedł na kanał.” (`0x823540`, `translateBNOwnerEnterChannel`). SteamNet podmienia go w naszym
`connect()` (wątek gry) na nazwę konta Steam.
Zapisane dane gracza (`/setplayerdata`, bohater RPG) są kluczowane identyfikatorem konta (`steam_<SteamID>`),
więc zmiana nazwy na Steam ich nie gubi; dane zapisane wcześniej pod nickiem są przenoszone przy pierwszym odczycie.

Gdy okno logowania jednak się pojawi: drugie słowo komunikatu błędu (`"tekst" "nick"`) trafia do kontrolki
`0x564` tylko wtedy, gdy okno jest już otwarte (`0x81F9C0`), a komunikat pokazuje się tylko przy zapamiętanym
loginie — dlatego SteamNet nie opiera logowania Steam na tym oknie.

### Odpowiedź na logowanie (`0x803820`)

Błąd: `i32 kod≠0`, `napis komunikat` → vfunc `+0x9C(kod, komunikat)`. Klient zostaje w stanie 7 z otwartym połączeniem:
okno logowania (`0x8259B0`, przycisk `0x3069`) wysyła kolejny pakiet logowania **tym samym połączeniem** (`0x819570`).
Komunikat to `"tekst" ["nick do pola nazwy"]` (tokenizer `0x829B60`); tekst jest formatem `String_Format` — bez `%`.
Pierwsze połączenie z listy serwerów ma pusty login i hasło: serwer odpowiada błędem, gra pokazuje okno logowania.

Sukces (`i32 0`), dalej po kolei:

| pole | gdzie w kliencie | uwagi |
|---|---|---|
| `u32` | `+0x4EC8` | |
| `napis` | `+0x4EB4` | tekst powitalny |
| `napis` | `+0x4EB4` | drugi raz to samo pole (zastępuje pierwszy); trafia do `+0x9C(0, …)` |
| `8 bajtów` | `+0x4EF0` | double |
| `u32` × 9 | `+0x4ED8`, `+0x4ECC`, `+0x4ED4`, `+0x4ED0`, `+0x4EDC`, `+0x4EE0`, `+0x4EF0`, `+0x4EE4`, `+0x4EE8` | |
| lista 1 | `+0x4EFC` / `+0x4F00` | powtarzane `{u8 indeks, napis}`, koniec = bajt `0xFF` |
| lista 2 | `+0x4F10` / `+0x4F14` | jw. |
| lista 3 | `+0x4F24` / `+0x4F28` | jw. — **kategorie rankingu**; `/ladder` bez ważnego indeksu nie wypełnia tabeli (SteamNet: `{0, "KnightShift"}`) |
| `u8` | `+0x4EEC` | |
| `napis` | `+0x4EB8` | **nazwa kanału startowego**; `+0x4EBC` zerowany |
| `u32 flaga`, `GUID` | obrazek/plik 1 | flaga 0 + GUID zerowy = pomiń (domyślny obrazek `+0x4A74`) |
| `u32 flaga`, `GUID` | obrazek/plik 2 | jw. |
| `64 bajty` | `0x806910` | **podpis** z tożsamości gracza (niżej). Zły → „nieprawidłowy numer seryjny” |

### Podpis (`0x8069E0`)

Wejście: 64-bitowa tożsamość z klucza CD (`0x937910`, ta sama co przy dołączaniu do gier), wyjście 64 bajty:

```
T[64] = d2 12 13 d3 11 d1 d0 10 f0 30 31 f1 33 f3 f2 32 36 f6 f7 37 f5 35 34 f4 3c fc fd 3d ff 3f 3e fe
        fa 3a 3b fb 39 f9 f8 38 28 e8 e9 29 eb 2b 2a ea ee 2e 2f ef 2d ed ec 2c e4 24 25 e5 27 e7 e6 26
s = 0; a = 0; x = 0
dla i = 0..7:  b = bajt i identyfikatora; s += x + b; x ^= b; a += x
dla i = 0..63: out[i] = T[(i + s) % 64] ^ bajt ((i + a) % 8) identyfikatora
srand(s + a)                      (rand MSVC: r = r*214013 + 2531011; (r >> 16) & 0x7FFF)
dla i = 0..63: out[i] ^= rand() & 0xFF
32 razy: p = rand() % 64; q = rand() % 64; zamień out[p], out[q]
```

Emulator działa w tym samym procesie, więc czyta tożsamość z `0x937910` (ex2: `0x943B50`) i liczy podpis sam.
`en::Sign` daje bajt w bajt to samo co funkcja gry uruchomiona w emulatorze x86 (unicorn) dla 4 tożsamości.

## Etap 2 — linie tekstu

Każda linia to napis ANSI zakończony bajtem `0`. Argumenty: słowa oddzielone spacją/tabem albo w
cudzysłowie `"…"`; w cudzysłowie działa dekodowanie `%XX` (`0x806D30`). Serwer nie powinien wysyłać `"` ani
`%` w argumentach (albo koduje je jako `%22`, `%25`).

Pierwszy znak linii serwera (`0x80E340`, skok po `znak − '$'`):

| prefiks | znaczenie |
|---|---|
| `$` | dodaj do listy |
| `&` | usuń z listy |
| `@` | zmień (tylko `play`) |
| `/` | komenda (`0x81172C`) |

### Listy

| linia | wywołanie | argumenty |
|---|---|---|
| `$user "nick" <liczba> "tekst" "guid"` | `+0xC8(nick, NULL, liczba, &guid)` | gracz wszedł na kanał; guid bez klamer `%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x` |
| `&user "nick" "dokąd"` | `+0xCC(nick, dokąd)` | wyszedł (na inny kanał / do gry) |
| `$channel "nazwa" x y "opis"` | `+0xBC(nazwa, opis)` | kanał na liście |
| `&channel "nazwa"` | `+0xD0(nazwa)` | |
| `$play "nazwa" x y <ipv4> "guid"` | `+0xC0(nazwa, 0,0,0,0)` | gra na liście; `ipv4` jako liczba dziesiętna (rekord +0x1C) |
| `@play "nazwa" <gracze> x <ipv4> y "poziom" <maks> <reszta>` | `+0xC4(nazwa, poziom, gracze, maks, reszta)` | opis „nazwa (gracze/maks - poziom)”; `ipv4` 0 = bez zmian |
| `&play "nazwa"` | `+0xD4(nazwa)` | tylko gdy nazwa jest na liście (dokładne porównanie) |

Rekord gry (`+0x4A64`, liczba `+0x4A68`, 0x34 B): +0x04 GUID, +0x14 nazwa, +0x1C IPv4, +0x24 poziom,
+0x2C gracze, +0x30 maks. Dołączenie z listy: `0x82E920` → `0x84BBB0`/`0x84C500(ipv4)`.

### Komendy serwera (`/`)

| linia | wywołanie / efekt |
|---|---|
| `/join "kanał" x y "tekst"` | czyści listę graczy, `+0x4EB8`=kanał, `+0x4EBC`=tekst, `+0xD8(kanał, tekst)` |
| `/send "nick" "tekst"` | `+0xAC(nick, tekst)` — wiadomość na kanale |
| `/msg x "od" y "tekst"` | `+0xB0(od, tekst, 0)` — szept od kogoś |
| `/msgc "do" "tekst"` | `+0xB0(do, tekst, 1)` — potwierdzenie własnego szeptu |
| `/admin reszta` | `+0xB4(reszta)` — komunikat administratora |
| `/error …` | `+0x9C` / `+0xAC` (do rozpisania) |
| `/getplayerdata "nick" "klucz" <n>` + `n` bajtów | `+0xE0(nick, klucz, dane, n)`; `n=0` = brak danych |
| `/bin <n>` + `n` bajtów | odczyt binarny (do rozpisania) |
| `/plays "nazwa" "hasło"` | serwer pozwala hostować: klient woła `+0xE4(0)` (hostuje sesję DirectPlay „EarthNetSession”) i odsyła `/plays "nazwa" "hasło" "<guid sesji>"` |
| `/playc …` | dołączanie do gry (`+0xE8`, `+0xC8`) — do rozpisania |
| `/ladder`, `/ladderm`, `/ladderw` `kat` wiersze… | ranking (`0x80A6F0`, potem `+0xDC`); szczegóły niżej |
| `/whois …` | informacje o graczu (`+0xF4`) |
| `/syncstats a b c d e f g` | liczniki (`+0xF8`): gracze zalogowani / wszyscy, gry otwarte / wszystkie, kanały, 2× nieużywane |
| `/info "tekst"` | `+0xB8` |
| `/nickok` | `+0xFC` |
| `/login …` (tekstowy) | tylko tryb 1 (nieużywany) |

### Ranking (`0x80A6F0`, ekran `0x82CF60`)

`/ladder` (ogólny), `/ladderm` (miesięczny), `/ladderw` (tygodniowy); `+0x29A8` = 0/1/2. Po komendzie:

- `kat` — indeks w liście 3 z odpowiedzi na logowanie (`+0x4F24`). Zły indeks lub pusta pozycja → tabela
  zostaje pusta (sam `+0xDC` i tak jest wołany).
- potem wiersze po **9 słów**: `"nick" "data" zwyc przegr rozl punkty x y z`
  - `data` — liczba dni od 1899-12-30 (data OLE, `sscanf "%lf"` → `VariantTimeToSystemTime`), pokazywana jako
    data i godzina lokalna;
  - `zwyc` `+0x214`, `przegr` `+0x218`, `rozl` `+0x21C`, `punkty` `+0x434` (ujemne → 0), `x` `+0x438`
    (nieużywane na ekranie), `y` i `z` pomijane.
- Koniec linii kończy tabelę. **Najwyżej 10 wierszy** (wiersz 0x428 B od `+0x14`; parser nie sprawdza granicy).

Ekran: Poz. = numer wiersza, Gracz, Punkty, Gry = zwyc + przegr + rozl, Zwyc., Przegr., Rozł., Ostatnia gra.
Własny nick (`+0x4A80`) jest wyróżniony kolorem. Sprawdzone emulacją (`analysis/steamnet_emu/emu_ladder.py`).

### Gdzie SteamNet trzyma ranking

Tablice wyników Steam (ISteamUserStats) aplikacji 254060, zakładane przez grę (`FindOrCreateLeaderboard`,
malejąco, liczbowo): `SteamNet` (cały czas), `SteamNet RRRR-MM` (miesiąc), `SteamNet RRRR-Wtt`
(tydzień ISO, UTC). Jeden wpis na konto Steam: wynik = punkty, szczegóły (int32):
`{1, zwyc, przegr, rozl, ostatnia gra (czas unix), nick w 16 bajtach}`. Pierwsze logowanie dopisuje gracza
do `SteamNet` z 0 punktami (tylko gdy go tam nie ma). Tablice miesięczne i tygodniowe są tylko czytane
(`FindLeaderboard`), dopóki nie ma wyników. Kod: `RankedBackend` (`earthnet_core.cpp`), `SteamRanking` (`steampeer.cpp`).

### Kanały online (lobby Steam)

Kanał SteamNet = **niewidoczne** lobby Steam (`k_ELobbyTypeInvisible`, do 250 osób) z danymi `ksnet=chan1`,
`chan=<nazwa małymi literami>`, `name=<nazwa>`. Steam pozwala być w jednym zwykłym lobby (sesja gry transportu,
`ksnf=1`) i dwóch niewidocznych naraz, a wyszukiwanie zwraca też niewidoczne. Wejście: szukanie po `chan`, dołączenie
do najliczniejszego lobby albo założenie nowego. Członkowie → `$user "nazwa Steam" 0 "" <zerowy guid>` (kolor „inny
gracz”; nazwy o tej samej treści dostają `#2`, `#3`), wyjście → `&user`. Czat: wiadomość lobby `"S" + tekst` →
`/send "nazwa" "tekst"`; własne wiadomości nie wracają, więc serwer pokazuje je od razu. Szept: Steam Networking
Messages, kanał 8, `"W" + tekst` → `/msg 0 "od" 0 "tekst"`; transport przyjmuje sesję od członków kanału. Lista
kanałów: wyszukiwanie `ksnet=chan1` co 60 s → `$channel` / `&channel`. `/syncstats a b c d e f g` = wszyscy
gracze, zalogowani, kanały, gry w toku, gry otwarte (`0x8239A0`: „gracze b/a, gry e/(d+e), kanały c”).
Kod: `OnlineBackend` (`earthnet_core.cpp`), `SteamLobbies` (`steampeer.cpp`); zdarzenia z wątku Steam idą przez
`LobbyEvents` do wątku połączenia (pętla `select` co 100 ms).

**Gry online.** Host: po `/plays "nazwa" "hasło" "guid"` (gra założona) SteamNet ustawia dane członka lobby kanału
`game = "<lobby gry transportu>\n<guid>\n<nazwa>"` (gdy transport już założył lobby gry); po `/join` (koniec gry)
czyści je. Pozostali: zmiana danych członka (`LobbyDataUpdate_t`) → gra dostaje wirtualny adres `10.83.x.y`
(bajty; w linii liczba dziesiętna jak w rekordzie `+0x1C`) → `$play "nazwa" 0 0 <ipv4> "guid"`; ta sama nazwa od
kilku graczy dostaje ` #2`. Wyjście gracza z kanału usuwa jego grę (`&play`).

**Dołączanie** (sprawdzone emulacją, `emu_playc_send.py`, `emu_playc.py`): klient `0x80BAD0` wysyła
`/playc "<guid gry>" "<nazwa>" "<hasło>"` i zapamiętuje IPv4 gry (`+0x4A48`). Odpowiedź
`/playc "<guid>" "<nazwa>" 1 <ipv4>` → `+0xE8(0, ipv4)` (`0x823E10`) → `0x82E920(nazwa, …, ipv4)` →
`0x84C500` / `0x84BBB0` z adresem `"%d.%d.%d.%d"` jako nazwą hosta DirectPlay. Trzecie słowo `0` →
`+0xE8(2)` „złe hasło”. Transport Steam (`ParseSteamTarget`) zamienia adres `10.83.x.y` na lobby gry i łączy
jak z `ks-lobby:<id>`.

### Komendy klienta

| linia | kiedy |
|---|---|
| `/getplayerdata "nick" "KS_RPG_ChData.1.0"` | zaraz po zalogowaniu (bohater RPG z serwera) |
| `/setplayerdata "nick" "klucz" "n"` + dane | zapis bohatera |
| `/join "kanał" ["hasło"]` | zmiana kanału (`+0x44`); także po wyjściu z pokoju gry — innej wiadomości o końcu gry nie ma |
| `/msg "#kanał" "tekst"` | wiadomość na kanale — tak wysyła okno czatu (serwer odpowiada `/send "nick" "tekst"`) |
| `/msg "nick" "tekst"` | szept (serwer odpowiada `/msgc "nick" "tekst"`) |
| `/send "tekst"` | wiadomość na kanale (`+0x48`, starsza ścieżka) |
| `/create "…"` | nowy kanał / konto (`+0x0C`) |
| `/whois "nick"`, `/update "nick" "" "4294967295" "" "255" "255" ""`, `/characterinfo "…" "%d"` | `/update` po wejściu na ekran główny — bez odpowiedzi |
| `/ladder`, `/ladderm`, `/ladderw` | ranking |
| `/plays "<zerowy guid>" "RTS : nazwa" "hasło"` | „Nowa gra RTS/RPG”: prośba o hostowanie; po `/plays` z serwera klient hostuje i rejestruje grę `/plays "nazwa" "hasło" "<guid>"` |
| `/playc …`, `/playi …`, `/playg …`, `/play0|v|d …`, `/newhost`, `/newbadhost` | gry: zakładanie, dołączanie, wynik, zmiana hosta |

Po zalogowaniu klient sam dodaje siebie do listy (`+0xC8`), wysyła `/getplayerdata` i przechodzi na
ekran główny EarthNet (ekran `0x0C`).

## Metody klienta (vtable `0x8FABA0`)

| offset | adres | rola |
|---|---|---|
| +0x04 | 0x7FF470 | init (blob „client information”, okno gniazda) |
| +0x08 | 0x7FF8D0 | połącz (host, port, login, hasło) |
| +0x0C | 0x81B6D0 | `/create` |
| +0x10 | 0x819570 | `/login` (tryb tekstowy / reconnect) |
| +0x44 | 0x808880 | `/join` |
| +0x48 | 0x809340 | wiadomość na kanale: `/msg "#kanał" "tekst"` |
| +0x4C | 0x8097D0 | szept: `/msg "nick" "tekst"` — okno czatu woła je, gdy wciśnięty jest przełącznik „Prywatnie” (kontrolka `0x56A`), z graczem zaznaczonym na liście (`+0x51D8`) |
| +0x50 | 0x809FE0 | ranking: `/ladder` / `/ladderm` / `/ladderw` (argument 0/1/2) |
| +0x54…+0x70 | 0x80B580…0x80DE10 | `/plays`, `/playc`, `/playi`, `/play0/v/d`, `/playg`, `/newhost`, `/join` |
| +0x7C | 0x80E340 | obsługa linii od serwera |
| +0x80 / +0x84 | 0x802130 / 0x8023C0 | wysłano / odebrano (maszyna stanów) |
| +0x88…+0x98 | | `/whois`, `/update`, `/getplayerdata`, `/setplayerdata`, `/characterinfo` |
| +0x9C | 0x81F9C0 | wynik logowania |
| +0xA0 / +0xA4 / +0xA8 | 0x820100 / 0x820260 / 0x8205F0 | błąd połączenia / błąd / utrata połączenia |
| +0xAC / +0xB0 / +0xB4 | 0x822C60 / 0x823170 / 0x823460 | wiadomość / szept / admin |
| +0xBC…+0xD4 | | listy (kanał, gra, gracz: dodaj / zmień / usuń) |
| +0xD8 | 0x823540 | wejście na kanał |
| +0xE0 | 0x823A30 | dane gracza (bohater RPG) |
| +0xE4 | 0x823CB0 | hostuj grę |
| +0xE8 | 0x823E10 | dołącz do gry / błąd |
| +0xF4 / +0xF8 / +0xFC | | whois / liczniki / nick OK |

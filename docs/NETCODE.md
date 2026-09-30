# KnightShift (Polanie II) – netcode po reverse engineeringu

Dotyczy wersji Steam 1.3 (build 2012-01-02): `KnightShift.ex1` (D3D8 „classic”) i
`KnightShift.ex2` (D3D8 z shaderami). Oba buildy mają identyczny kod sieci; adresy
poniżej dotyczą **ex1** (adresy obu buildów: tablice `kEx1` / `kEx2` w `src/ksnetfix.cpp`; w ex2 globalne sieci są
przesunięte o `+0x5CB540`, zegar/pętla o `+0xC240`, RNG o `+0x9788`).

Silnik to Earth 2150 Engine (Reality Pump) — mutex nazywa się `"Earth 2150"`,
lobby to EarthNet (`netserver.earthnet.de`, martwe).

## 1. Warstwy

```
 DirectPlay 8 Peer (dpnet.dll, CLSID_DirectPlay8Peer, SP TCP/IP)     <- transport UDP (+ dpnsvr.exe do enumeracji, port 6073)
   │  Net_DPMessageHandler 0x7EB890 (wątki DirectPlay): RECEIVE -> kolejka g_rxQueue
   ▼
 Net_Pump 0x7EEA10 (wątek symulacji, raz na tick, tylko tryb sieciowy)
   ├─ dispatch g_rxQueue po klasie wiadomości (pierwsze słowo)
   ├─ Net_ExecIncoming 0x7F0BE0  -> TurnIn::Process(server), TurnIn::Process(client)
   ├─ Net_FlushOutgoing 0x7F0BB0 -> TurnOut::Flush(server),   TurnOut::Flush(client)
   └─ Net_UpdateWaitingStatus 0x7F3430 ("Waiting for commands", "Waiting for player %s")
```

Klasy wiadomości (dword 0 bufora DirectPlay):

| klasa | znaczenie | obsługa |
|---|---|---|
| 0 | czat / wiadomość użytkownika | callback `0xF62224` |
| 1–5 | transfer plików (mapy, save do dołączania w trakcie) — oryginalna klasa `CInNet` | `0x7F8E10` |
| 6–7 | potwierdzenia transferu — oryginalna klasa `COutNet` | `0x7F6C90` |
| 0x0B | pakiet gry klient → host | `TurnIn_OnPacket(g_turnServerIn)` — od razu na wątku DirectPlay |
| 0x0C | pakiet gry host → gracze | `TurnIn_OnPacket(g_turnClientIn)` — w pompie |
| 0x0D | żądanie retransmisji zakresu | `TurnOut_ResendRange` |
| 0x0E | info o graczu | `0x7F1060` (host) / `0x7F1750` (klient) |
| 0x0F | skompresowany blok stanu sesji (prawdopodobnie archiwum/replay) | `0x7FBD90` |
| 0x11 | dołączanie w trakcie gry (dynamic connection) | `0x7EA390` |

Debug-stringi `CInNet::…` / `COutNet::…` należą do **transferu plików**, nie do tur.
Obiekty tur nazwałem `TurnIn` / `TurnOut`:

| globalna | rola |
|---|---|
| `g_turnServerIn`  `0xF62428` | host: rozkazy od graczy |
| `g_turnClientIn`  `0xF6242C` | każdy (host też, przez loopback): tury od hosta |
| `g_turnServerOut` `0xF62430` | host → wszyscy |
| `g_turnClientOut` `0xF62434` | gracz → host |

## 2. Format pakietu gry (klasy 0x0B / 0x0C)

```
+0x00 u32  klasa (0x0B / 0x0C)
+0x04 u32  numer sekwencyjny (ostatnia wiadomość w pakiecie)
+0x08 u32  g_turnsExecuted nadawcy
+0x0C u32  g_syncHash   — w oryginale ZAWSZE 0xFFFFFFFF (kod checksumy wycięty)
+0x10 u16  długość payloadu
+0x12 u8   liczba wiadomości w pakiecie (nowa + redundantne kopie poprzednich)
+0x13 ...  skompresowany strumień: { u32 player, u32 len, u8 data[len] } × n
```

Wysyłka: `SendTo(..., flags 0x90)` = **niegwarantowane**, wysoki priorytet. Niezawodność
jest własna: redundancja (każdy datagram niesie ostatnie N wiadomości), żądanie
retransmisji 0x0D przy dziurze w numeracji, oraz okresowy resend (`TurnOut_ServerResend`
0x7FC250, `TurnOut_ClientKeepAlive` 0x7FC090) co `turnLen` ticków. Kolejka wysyłki
`TurnOut` ma 21 slotów (pełna → wiadomość odrzucana, flaga 0x10).

Wiadomości 1-bajtowe sterujące:

| bajt | znaczenie |
|---|---|
| `0xFF` | znacznik tury (host) |
| `0xFD` | znacznik tury + wejście w pauzę |
| `0xFB` | znacznik tury + wyjście z pauzy |
| `0xF9` / `0xFA` | znacznik tury + włączenie / wyłączenie stanu `0xF62504` (prawdopodobnie zapis gry sieciowej) |
| `0xFE` | ACK postępu klienta |
| `0xFC` | gotowość gracza (ustawia bit w `g_readyMask`) |

## 3. Model czasu i lockstep

* Wątek symulacji `SimThread_Main` (0x4062C0) tyka z częstotliwością `g_ticksPerSecond` = 30
  (opcja GameRate 25–35). Licznik `g_realTick` (0x9372B0) rośnie **zawsze**.
* W trybie sieciowym harmonogram jest absolutny (`g_nextTickTime += g_tickPeriod`), więc
  lokalne przycięcia są nadrabiane seriami ticków (do 1 s bez renderowania).
* Krok świata `Sim_Tick` (0x480110) wykonuje się tylko, gdy
  `g_realTick < g_turnBase + g_turnLen && !g_netPaused`.
* `g_turnLen = floor(rate/30 × g_turnLenBase)`, `g_turnLenBase = 5` → tura = 5 ticków = **167 ms**.
* **Host** (`TurnOut_Flush` 0x7F97A0, obiekt serwerowy) generuje znacznik tury, gdy:
  - minęło `g_turnLen` ticków od poprzedniego,
  - każdego gracza słyszał w ciągu `g_maxTurnsAhead` (= 3) tur (`g_playerLastHeardTurn`),
  - rozrzut wykonanych tur między graczami ≤ `g_maxTurnSpread` (= 4), a jego własna kolejka < 2 znaczników.
  Rozkazy graczy są od razu przekazywane (relay) do strumienia hosta.
* **Każdy gracz** (`TurnIn_Process` 0x7FB2E0) wykonuje zebrane rozkazy przy znaczniku tury —
  **najwyżej jeden znacznik na tick i tylko gdy `g_realTick >= g_turnBase + g_turnLen`**,
  po czym `g_turnBase = g_realTick`.
* Inwariant deterministyczny: dokładnie `g_turnLen` kroków symulacji między dwoma znacznikami.
* Klient potwierdza postęp (`0xFE`) co `(g_maxTurnsAhead >> 1) + 1` = 2 tury, jeśli nie ma nic innego do wysłania.
* Parametry 5/5/3/4 są na sztywno; ścieżka „debug override” (`g_useDebugNetParams`, 0x92AF2C..) jest martwa.

## 4. Znalezione problemy synchronizacji

1. **Brak nadrabiania tur.** Spóźniony znacznik = tick bez symulacji, a `turnBase = now` gubi ten
   czas na zawsze. Klient, który raz zostanie w tyle (jitter, zacięcie, wolniejszy zegar), zostaje
   z trwałą kolejką tur → każdy rozkaz czeka dodatkowo `kolejka × 167 ms`, aż host zdławi grę
   (throttle przy rozrzucie > 4).
2. **Zegar RDTSC z błędną kalibracją.** `Clock_Read` = `rdtsc`, kalibracja 101× po 0,1 ms okna QPC,
   średnia z 5 najmniejszych → zaniżenie i rozrzut ~0,05–0,2 % między PC. Host dyktuje tempo,
   więc klient z „wolniejszym” zegarem systematycznie traci tury.
3. **Przepełnienie 32-bit.** `(tscPerMs × 1000) / rate` liczone w 32 bitach — przy TSC > 4,29 GHz
   okres ticku jest błędny (gra działa w złym tempie).
4. **Granulacja timera Windows 15,6 ms** przy 33,3 ms tickach — gra nie wywołuje `timeBeginPeriod`,
   więc pojedyncze ticki mają ±15 ms jittera (większa szansa na „spóźnione” tury).
5. **Tolerancja jittera ~1 tura.** ACK co 2 tury przy limicie 3 tur daje ~167 ms zapasu na wahania
   opóźnienia; powyżej host staje („Waiting for player…”).
6. **Brak wykrywania desynchronizacji.** Pole `syncHash` w nagłówku i mapy `g_mapTurnToRemoteHash`
   (zbierane co 16 tur, `g_syncCheckEnabled = 1`) istnieją, ale producent i porównanie zostały
   wycięte — rozjechane gry nigdy nie są wykrywane.
7. **RNG współdzielony z renderem (do potwierdzenia pomiarem).** 8 strumieni LCG `x*0x343FD+0x269EC3`
   w `0x928670..0x928690`. Render i symulacja wykluczają się semaforem `g_simRenderSem`, ale jeśli
   render losuje z tego samego strumienia co symulacja, liczba wywołań zależy od FPS → desync.
   KSNetFix mierzy to w grze (`RngMonitor`) i izoluje (`RngIsolation`).

## 5. KSNetFix — co zmienia (`src/`)

| poprawka | problem | zgodność |
|---|---|---|
| PreciseClock: zegar z QPC w ns, 1 000 000 jednostek/ms | 2, 3 | lokalne |
| TimerResolution: `timeBeginPeriod(1)` | 4 | lokalne |
| CatchUp (od 2.4 płynny, `CatchUpSmooth=1`): tick trwa `okres / tempo`, gdzie tempo 0,95–1,25× wynika ze średniej długości kolejki znaczników (średnia krocząca, cel `SmoothTarget` = 0,25 tury). Ticki są równo rozłożone, więc ruch jednostek u dołączonego gracza jest płynny. Przy kolejce ≥ 3 tur (zacięcie, wczytywanie) i w trybie `CatchUpSmooth=0` (2.2/2.3) pętla dostaje dodatkowe pełne ticki (do 3×). Pełny tick bez renderu wykonuje dwa kroki symulacji w jednej klatce, a potem klient czeka na następną turę, co gracz widzi jako szarpany ruch („jak 30 fps”). | 1 | lokalne, deterministyczne (liczba ticków na turę bez zmian) |
| AckEveryTurns=1 | 5 | protokół bez zmian |
| MaxTurnsAhead 3→5, MaxTurnSpread 4→6 (host) | 5 | tylko host |
| RngIsolation + RngMonitor | 7 | tylko w grze sieciowej; najlepiej u wszystkich |
| DesyncCheck: hash RNG symulacji w polu `syncHash` (tag = tura, której dotyczy), porównanie u odbiorcy; domyślnie tylko strumienie 0–5 — 6/7 (`0x68C`/`0x690`) karmią efekty cząsteczkowe (`0x724860`) zależne od kamery gracza | 6 | gracze bez poprawki wysyłają 0xFFFFFFFF i są pomijani |
| NetTrace: przywrócony oryginalny trace sieci deweloperów → `ksnettrace.log` | diagnostyka | lokalne |

Czego świadomie NIE zmieniono: długość tury (`g_turnLenBase`) — musi być identyczna u wszystkich,
inaczej natychmiastowy desync. Skrócenie tury z 167 ms do 67–100 ms dałoby kolejne ~40–80 ms mniej
opóźnienia rozkazów, ale wymaga negocjacji wartości przez hosta przed pierwszą turą (patrz „Dalsze kroki”).

## 6. Dalsze kroki

* Negocjowana długość tury (host ogłasza w nagłówku, klienci przyjmują przed turą 1, gracze bez
  poprawki blokują opcję).
* Automatyczny resync po wykryciu desynchronizacji przez istniejący mechanizm „dynamic connection”
  (host zapisuje grę i wysyła ją graczowi — `Net_DynamicConnection` 0x7EA390).
* ~~Zastąpienie DirectPlay dla NAT/relay~~ — zrobione w KSNetFix 2.0 (`src/steampeer.cpp`):
  własna implementacja `IDirectPlay8Peer` (clean-room, `directplay-lite` jest na GPL-2 i nie da się
  jej łączyć ze `steam_api.dll`) podstawiana przez hook `CoCreateInstance` w IAT gry. Gra używa
  17 metod peera i 12 komunikatów; odkrycie sesji = lobby Steam, ruch = Steam Networking Messages
  (kanał 7, niezawodne dla wysyłek `GUARANTEED`, reszta nieniezawodne), topologia pełnej siatki.

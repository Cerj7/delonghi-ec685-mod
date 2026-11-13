# Contract: Main Board FSM

Стани та обов’язкові дії (актуатори + кадри панелі). Числа — через константи з [tunable-constants.md](./tunable-constants.md).

Оновлено після clarify 2026-11-12 (session 4): вихід з пару лише `B1`/`B2`; cold flush на `B1`; `STEAM_COOL` + ручка → імпульси / ТЕН OFF; heat-up ПІ; idle/`B3`-cancel вимкнено.

## States

```
POWER_ON
   │ HE1, target = espresso pot
   ▼
ESPRESSO_HEAT ──ready(Tt ≥ target−READY_BAND_C, READY_HOLD_MS; no upper)──► ESPRESSO_READY (HE0)
   ▲                                                      │
   │ heater PI: heat-up                                   ├─ B1 → COLD_FLUSH (P11)
   │                                                      ├─ B2 → BREW_2 (P21)
   │                                                      └─ B3 → enterSteam()
   │                                                            // Tt < STEAM_TARGET_C → STEAM_HEAT (HS1)
   │                                                            // Tt ≥ STEAM_TARGET_C → STEAM_READY (HS0)
   │
COLD_FLUSH
   pump: COLD_FLUSH_PCT for COLD_FLUSH_MS (or until B1)
   heater: FORCED OFF
   stop: timeout or B1 → P10 → ESPRESSO_READY
   ignore: B2, B3
   // MUST NOT start if already in BREW_2 / HS1 / SS1 / STEAM_COOL
   // (HS0/SS0: B1 is steam-exit → coolOrReady, not flush)

BREW_2
   phases: PREINFUSE → PAUSE → FLOW
   heater PI: PREINFUSE/PAUSE → base; FLOW → brew-flow (~5× + FF)
   stop: B2 → P20 → ESPRESSO_READY
   ignore: B1, B3

STEAM_HEAT (HS1)
   ├─ Tt ≥ STEAM_TARGET_C → STEAM_READY (HS0)  // no READY_HOLD_MS; no upper band
   │     if handle open → immediately STEAM_FLOW (SS1)
   ├─ B1 or B2 → coolOrReady()                 // ONLY exit to LT1
   └─ B3 → ignore
   heater PI: heat-up

STEAM_READY (HS0)
   ├─ maintain target STEAM_TARGET_C
   ├─ Tt < STEAM_TARGET_C → STEAM_HEAT (HS1)   // auto reheat ONLY when handle closed
   ├─ handle open → STEAM_FLOW (SS1)
   ├─ B1 or B2 → coolOrReady()                 // ONLY exit to LT1
   └─ B3 → ignore                              // NOT cancel, NOT SS1
   // NO idle auto-exit (STEAM_IDLE_EXIT_MS=0)
   heater PI: base

STEAM_FLOW
   pulse pump STEAM_PULSE_ON_MS / OFF @ STEAM_PULSE_PCT  // continuous while handle open
   heater PI: steam-flow (~4× + FF); keep target STEAM_TARGET_C
   Tt < STEAM_TARGET_C → STAY in STEAM_FLOW   // NO HS1, NO SS0, NO pump pause (FR-026)
   handle close → SS0, pump 0 → STEAM_READY (HS0)        // NOT coolOrReady
   B1 or B2 → coolOrReady()
   B3 → ignore

STEAM_COOL (LT1)
   target orient = espresso pot (for ready criterion)
   heater: FORCED OFF
   handle open → pulse pump (same STEAM_PULSE_*)         // FR-028
   handle closed → pump 0
   ready criterion → pump 0 (even if handle open) → LT0 → ESPRESSO_READY
   ignore B1/B2 (no cold flush / no brew)
   B3 → enterSteam()   // FR-025: Tt ≥ target → HS0; else HS1
   panel LT1 = protocol LED3 double-blink (no LED1+LED2 requirement)

FAULT
   heater OFF, pump 0, OLED fault
   exit v1: power cycle only
```

## Helper: `enterSteam()`

Вхід / повернення в пар (з `ESPRESSO_READY` або `STEAM_COOL` через `B3`):

1. `activeTargetC = STEAM_TARGET_C`
2. Якщо `Tt ≥ STEAM_TARGET_C` → надіслати `HS0`, увійти в `STEAM_READY`; якщо ручка вже відкрита → одразу `SS1` / `STEAM_FLOW`
3. Інакше → надіслати `HS1`, увійти в `STEAM_HEAT`

## Helper: `coolOrReady()`

Вихід з пару через **`B1` або `B2`** у `HS1` / `HS0` / `STEAM_FLOW` (і будь-який `SS0` у паровому утриманні):

1. Зупинити парову імпульсну помпу (якщо активна)
2. `activeTargetC = espressoTargetC` (орієнтир для ready; підтримка ≥120 припиняється)
3. Надіслати `LT1` (якщо ще не в cool), увійти в `STEAM_COOL` з **ТЕН OFF**
4. Далі логіка `STEAM_COOL` (імпульси від ручки; ready → `LT0`)

Якщо на момент виклику Tt уже не вища за уставку еспресо — все одно шлях cool з `LT1` за бажанням UX «буде LT1», або короткий `LT1`→ready→`LT0`; імплементація MAY одразу `LT0`, якщо ready criterion уже виконано (стенд).

**Не** викликати з закриття ручки (`SS0`), **не** з idle-таймера, **не** з `B3`, **не** з падіння Tt під час `STEAM_FLOW`.

## Ready criteria (espresso)

**`HE0` (heat-up / `ESPRESSO_HEAT`)**:  
`Tt ≥ espressoTargetC − READY_BAND_C` continuously for `READY_HOLD_MS`.  
**Без верхньої межі** — overshoot над уставкою валідний.

**`LT0` (cool-down / `STEAM_COOL`)**:  
`Tt ≤ espressoTargetC + READY_BAND_C` continuously for `READY_HOLD_MS`.  
Одностороння верхня межа (дзеркало `HE0`); не ділити критерій `HE0`, інакше гарячий Tt одразу дав би `LT0`.

## Steam ready (`HS0`)

`Tt ≥ STEAM_TARGET_C` — одразу `HS0`. **Без** `READY_HOLD_MS`. **Без** симетричної смуги ±`STEAM_READY_BAND_C`.

У `STEAM_READY` з **закритою** ручкою: падіння нижче target → `HS1`, потім знову `HS0` (+ `SS1`, якщо ручка вже відкрита на момент `HS0`).

У `STEAM_FLOW`: падіння нижче target **не** змінює стан/панель/помпу — лише нагрів steam-flow профілем.

Idle-автовихід: **вимкнено** (`STEAM_IDLE_EXIT_MS=0`).

## Heater PI profile selection (FR-027)

| Context | Profile | Defaults (rel. to base) |
| --- | --- | --- |
| `ESPRESSO_HEAT` / `STEAM_HEAT` | heat-up `HEATER_KP_HEAT`/`HEATER_KI_HEAT` | > base; overshoot ~+3…5 °C OK; FF=0 |
| Idle / ready / brew PREINFUSE / brew PAUSE / `STEAM_READY` | base `HEATER_KP`/`HEATER_KI` | 5.0 / 0.05; FF=0 |
| `BREW_2` phase `FLOW` only | brew-flow + `HEATER_FF_BREW_PCT` | 25.0 / 0.25 (~5× stand) + FF 25 % |
| `STEAM_FLOW` | steam-flow + `HEATER_FF_STEAM_PCT` | 20.0 / 0.20 (~4× stand) + FF 20 % |
| `COLD_FLUSH` / `STEAM_COOL` / `FAULT` | forced OFF | heaterPercent=0 |

Цілі без offset. Feed-forward — адитивний % до виходу ПІ лише під час brew-`FLOW` / `STEAM_FLOW`, потім clamp 0…100 %. Перемикання ≤ одного `CONTROL_INTERVAL_MS`. `SAFETY_CUTOFF_C` спільний. Критерії `HE0`/`HS0` не послаблюються.

## Button matrix (must)

| State | B1 | B2 | B3 |
| --- | --- | --- | --- |
| ESPRESSO_HEAT | ignore | ignore | ignore |
| ESPRESSO_READY | cold flush | brew 2 | enterSteam |
| COLD_FLUSH | stop early | ignore | ignore |
| BREW_2 | ignore | stop | ignore |
| STEAM_HEAT | coolOrReady | coolOrReady | ignore |
| STEAM_READY | coolOrReady | coolOrReady | ignore |
| STEAM_FLOW | coolOrReady | coolOrReady | ignore |
| STEAM_COOL | ignore | ignore | enterSteam |
| FAULT | ignore | ignore | ignore |

## Pump authority

| Context | Pump % |
| --- | --- |
| COLD_FLUSH | `COLD_FLUSH_PCT` (100) until timeout/stop |
| PREINFUSE (`BREW_2`) | `PREINFUSION_PCT` |
| PAUSE / idle / heat / ready (no pulse) | 0 |
| FLOW (`BREW_2`) | pot `A0` (live) |
| STEAM_FLOW / STEAM_COOL ON phase (handle open) | `STEAM_PULSE_PCT` |
| STEAM_FLOW / STEAM_COOL OFF phase | 0 |
| STEAM_FLOW even if `Tt < target` | pulse continues (no forced 0 from temp) |
| STEAM_COOL at espresso ready | 0 even if handle still open → then `LT0` |

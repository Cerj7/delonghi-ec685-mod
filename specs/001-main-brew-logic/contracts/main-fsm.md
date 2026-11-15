# Contract: Main Board FSM

Стани та обов’язкові дії (актуатори + кадри панелі). Числа — через константи з [tunable-constants.md](./tunable-constants.md).

Оновлено після clarify 2026-11-15: dual steam targets (120 / 128), pot ranges, OLED layouts; session 4 exit/cold flush без змін.

## States

```
POWER_ON
   │ HE1, target = espresso pot (TARGET_MIN_C…TARGET_MAX_C)
   ▼
ESPRESSO_HEAT ──ready(Tt ≥ target−READY_BAND_C, READY_HOLD_MS; no upper)──► ESPRESSO_READY (HE0)
   ▲                                                      │
   │ heater PI: heat-up                                   ├─ B1 → COLD_FLUSH (P11)
   │                                                      ├─ B2 → BREW_2 (P21)
   │                                                      └─ B3 → enterSteam()
   │                                                            // Tt < STEAM_READY_TARGET_C → STEAM_HEAT (HS1)
   │                                                            // Tt ≥ STEAM_READY_TARGET_C → STEAM_READY (HS0)
   │
COLD_FLUSH
   pump: COLD_FLUSH_PCT for COLD_FLUSH_MS (or until B1)
   heater: FORCED OFF
   OLED: "Cold flush N s"  // N = ceil(remainingMs/1000); FR-033
   stop: timeout or B1 → P10 → ESPRESSO_READY
   ignore: B2, B3
   // MUST NOT start if already in BREW_2 / HS1 / SS1 / STEAM_COOL
   // (HS0/SS0: B1 is steam-exit → coolOrReady, not flush)

BREW_2
   phases: PREINFUSE → PAUSE → FLOW
   heater PI: PREINFUSE/PAUSE → base; FLOW → brew-flow (+ FF)
   pump FLOW: pot A0 mapped to PUMP_MIN_PCT…PUMP_MAX_PCT
   OLED: Tt / setpoint / heater% / pump%   // FR-008
   stop: B2 → P20 → ESPRESSO_READY
   ignore: B1, B3

STEAM_HEAT (HS1)
   target: STEAM_READY_TARGET_C (120)
   ├─ Tt ≥ STEAM_READY_TARGET_C → STEAM_READY (HS0)  // no READY_HOLD_MS; no upper band
   │     if handle open → immediately STEAM_FLOW (SS1, target 128)
   ├─ B1 or B2 → coolOrReady()                 // ONLY exit to LT1
   └─ B3 → ignore
   heater PI: heat-up
   OLED: Tt / heater% / open|close            // FR-029

STEAM_READY (HS0)
   ├─ maintain STEAM_READY_TARGET_C (120)
   ├─ Tt < STEAM_READY_TARGET_C → STEAM_HEAT (HS1)   // auto reheat ONLY when handle closed
   ├─ handle open → STEAM_FLOW (SS1), target = STEAM_FLOW_TARGET_C (128)
   ├─ B1 or B2 → coolOrReady()
   └─ B3 → ignore
   // NO idle auto-exit (STEAM_IDLE_EXIT_MS=0)
   heater PI: base
   OLED: Tt / heater% / open|close

STEAM_FLOW
   pulse pump STEAM_PULSE_ON_MS / OFF @ STEAM_PULSE_PCT
   activeTargetC = STEAM_FLOW_TARGET_C (128)
   heater PI: steam-flow (+ FF)
   Tt < STEAM_FLOW_TARGET_C (or even < 120) → STAY in STEAM_FLOW   // NO HS1, NO SS0, NO pump pause
   handle close → SS0, pump 0, target = STEAM_READY_TARGET_C (120) → STEAM_READY
   B1 or B2 → coolOrReady()
   B3 → ignore
   OLED: Tt / heater% / open|close

STEAM_COOL (LT1)
   target orient = espresso pot (for ready criterion)
   heater: FORCED OFF
   handle open → pulse pump (same STEAM_PULSE_*)
   handle closed → pump 0
   ready criterion → pump 0 (even if handle open) → LT0 → ESPRESSO_READY
   ignore B1/B2
   B3 → enterSteam()
   OLED: Tt / heater%(=0) / open|close         // FR-029 extends to cool
   panel LT1 = protocol LED3 double-blink

FAULT
   heater OFF, pump 0, OLED fault
   exit v1: power cycle only
```

## Helper: `enterSteam()`

Вхід / повернення в пар (з `ESPRESSO_READY` або `STEAM_COOL` через `B3`):

1. `activeTargetC = STEAM_READY_TARGET_C` (120)
2. Якщо `Tt ≥ STEAM_READY_TARGET_C` → надіслати `HS0`, увійти в `STEAM_READY`; якщо ручка вже відкрита → одразу `SS1` / `STEAM_FLOW` з `activeTargetC = STEAM_FLOW_TARGET_C` (128)
3. Інакше → надіслати `HS1`, увійти в `STEAM_HEAT`

## Helper: `coolOrReady()`

Вихід з пару через **`B1` або `B2`** у `HS1` / `HS0` / `STEAM_FLOW`:

1. Зупинити парову імпульсну помпу (якщо активна)
2. `activeTargetC = espressoTargetC` (підтримка парових цілей 120/128 припиняється)
3. Надіслати `LT1`, увійти в `STEAM_COOL` з **ТЕН OFF**
4. Далі логіка `STEAM_COOL` (імпульси від ручки; ready → `LT0`)

**Не** викликати з закриття ручки (`SS0`), **не** з idle-таймера, **не** з `B3`, **не** з падіння Tt під час `STEAM_FLOW`.

## Ready criteria (espresso)

**`HE0`**: `Tt ≥ espressoTargetC − READY_BAND_C` continuously for `READY_HOLD_MS`. Без верхньої межі.

**`LT0`**: `Tt ≤ espressoTargetC + READY_BAND_C` continuously for `READY_HOLD_MS`.

## Steam ready (`HS0`)

`Tt ≥ STEAM_READY_TARGET_C` (120) — одразу `HS0`. Без hold / без верхньої смуги.

У `STEAM_READY` (ручка закрита): падіння нижче 120 → `HS1`, потім знову `HS0` (+ `SS1` з ціллю 128, якщо ручка вже відкрита).

У `STEAM_FLOW`: ціль 128; падіння Tt **не** змінює стан/панель/помпу — лише нагрів steam-flow.

Idle-автовихід: **вимкнено** (`STEAM_IDLE_EXIT_MS=0`).

## Heater PI profile selection (FR-027)

| Context | Profile | Notes |
| --- | --- | --- |
| `ESPRESSO_HEAT` / `STEAM_HEAT` | heat-up | > base; FF=0; Kp/Ki = стенд у прошивці |
| Idle / ready / brew PREINFUSE / PAUSE / `STEAM_READY` | base | FF=0 |
| `BREW_2` phase `FLOW` only | brew-flow + FF | стенд у прошивці |
| `STEAM_FLOW` | steam-flow + FF | стенд; target 128 |
| `COLD_FLUSH` / `STEAM_COOL` / `FAULT` | forced OFF | heaterPercent=0 |

Цілі без offset. Перемикання ≤ одного `CONTROL_INTERVAL_MS`. `SAFETY_CUTOFF_C` > 128. Критерії `HE0`/`HS0` не послаблюються. **Не відкочувати** стендові Kp/Ki/FF.

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
| FLOW (`BREW_2`) | pot `A0` → `PUMP_MIN_PCT`…`PUMP_MAX_PCT` (live) |
| STEAM_FLOW / STEAM_COOL ON phase (handle open) | `STEAM_PULSE_PCT` |
| STEAM_FLOW / STEAM_COOL OFF phase | 0 |
| STEAM_FLOW even if `Tt < STEAM_FLOW_TARGET_C` | pulse continues |
| STEAM_COOL at espresso ready | 0 even if handle still open → then `LT0` |

## OLED authority

| Context | Content |
| --- | --- |
| `BREW_2` | Tt, setpoint, heater%, pump% |
| `COLD_FLUSH` | `Cold flush N s` (countdown) |
| `STEAM_HEAT` / `STEAM_READY` / `STEAM_FLOW` / `STEAM_COOL` | Tt, heater%, `open`\|`close` |
| `FAULT` | fault |

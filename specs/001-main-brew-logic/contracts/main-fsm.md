# Contract: Main Board FSM

Стани та обов’язкові дії (актуатори + кадри панелі). Числа — через константи з [tunable-constants.md](./tunable-constants.md).

Оновлено після clarify 2026-11-11 (session 3): `STEAM_FLOW` не паузить помпу при Tt < target; три ПІ-профілі (base / brew-flow / steam-flow).

## States

```
POWER_ON
   │ HE1, target = espresso pot
   ▼
ESPRESSO_HEAT ──ready(±READY_BAND_C, READY_HOLD_MS)──► ESPRESSO_READY (HE0)
   ▲                                                      │
   │                                                      ├─ B1 → BREW_1 (P11)
   │                                                      ├─ B2 → BREW_2 (P21)
   │                                                      └─ B3 → enterSteam()
   │                                                            // Tt < STEAM_TARGET_C → STEAM_HEAT (HS1)
   │                                                            // Tt ≥ STEAM_TARGET_C → STEAM_READY (HS0)
   │
BREW_1 / BREW_2
   phases: PREINFUSE → PAUSE → FLOW
   heater PI: PREINFUSE/PAUSE → base; FLOW → brew-flow (4× + FF)
   stop: same button → P10/P20 → ESPRESSO_READY
   ignore: other B*, steam button

STEAM_HEAT (HS1)
   ├─ Tt ≥ STEAM_TARGET_C → STEAM_READY (HS0)  // no READY_HOLD_MS; no upper band
   │     if handle open → immediately STEAM_FLOW (SS1)   // enter/resume with open handle
   └─ B3 → cancel → coolOrReady()
   heater PI: base

STEAM_READY (HS0)
   ├─ maintain target STEAM_TARGET_C (handle closed)
   ├─ Tt < STEAM_TARGET_C → STEAM_HEAT (HS1)   // auto reheat ONLY when handle closed; NOT coolOrReady
   ├─ handle open → STEAM_FLOW (SS1)
   ├─ idle STEAM_IDLE_EXIT_MS (default 3000), handle closed, no buttons
   │     → STEAM_COOL (LT1), drop steam target
   └─ B3 → cancel → coolOrReady()   // NOT SS1
   ignore B1/B2
   heater PI: base

STEAM_FLOW
   pulse pump STEAM_PULSE_ON_MS / OFF @ STEAM_PULSE_PCT  // continuous while handle open
   heater PI: steam-flow (2.5× + FF); keep target STEAM_TARGET_C
   Tt < STEAM_TARGET_C → STAY in STEAM_FLOW   // NO HS1, NO SS0, NO pump pause (FR-026)
   handle close → SS0, pump 0 → STEAM_READY (HS0)        // NOT coolOrReady; start/continue idle
   ignore B1/B2; B3 ignore during SS1

STEAM_COOL (LT1)
   target = espresso pot
   ready criterion → LT0 → ESPRESSO_READY
   ignore B1/B2
   B3 → enterSteam()   // FR-025: Tt ≥ target → HS0; else HS1
   heater PI: base

FAULT
   heater OFF, pump 0, OLED fault
   exit v1: power cycle only
```

## Helper: `enterSteam()`

Вхід / повернення в пар (з `ESPRESSO_READY` або `STEAM_COOL` через `B3`):

1. `activeTargetC = STEAM_TARGET_C`
2. Якщо `Tt ≥ STEAM_TARGET_C` → надіслати `HS0`, увійти в `STEAM_READY` (скинути idle-таймер); якщо ручка вже відкрита → одразу `SS1` / `STEAM_FLOW`
3. Інакше → надіслати `HS1`, увійти в `STEAM_HEAT`

## Helper: `coolOrReady()`

Лише скасування пару (`B3` у `HS1` / `HS0`) або idle-вихід (внутрішньо після `STEAM_IDLE_EXIT_MS`):

1. `activeTargetC = espressoTargetC`
2. Якщо `Tt > espressoTargetC` → надіслати `LT1` (якщо ще не в cool), увійти в `STEAM_COOL`
3. Інакше → надіслати `LT0` (зняти індикацію пару); якщо `Tt` ще нижче уставки → `HE1` / `ESPRESSO_HEAT`, інакше `ESPRESSO_READY`

**Не** викликати з закриття ручки (`SS0`) і **не** з падіння Tt під час `STEAM_FLOW`.

## Ready criterion (espresso)

`|Tt - espressoTargetC| ≤ READY_BAND_C` continuously for `READY_HOLD_MS`  
(Використовується для `HE0` і для `LT0`.)

## Steam ready (`HS0`)

`Tt ≥ STEAM_TARGET_C` — одразу `HS0`. **Без** `READY_HOLD_MS`. **Без** симетричної смуги ±`STEAM_READY_BAND_C` (Tt > target, у т.ч. >122 °C — валідна готовність).

У `STEAM_READY` з **закритою** ручкою: падіння нижче target → `HS1`, потім знову `HS0` (+ `SS1`, якщо ручка вже відкрита на момент `HS0`).

У `STEAM_FLOW`: падіння нижче target **не** змінює стан/панель/помпу — лише нагрів steam-flow профілем.

Idle: `STEAM_IDLE_EXIT_MS` (дефолт **3000**) рахується лише при **закритій** ручці без подій кнопок; відкрита ручка / `STEAM_FLOW` скидає або призупиняє таймер.

## Heater PI profile selection (FR-027)

| Context | Profile | Defaults (rel. to base) |
| --- | --- | --- |
| Idle / heat / ready / cool / brew PREINFUSE / brew PAUSE | base `HEATER_KP`/`HEATER_KI` | 5.0 / 0.05; FF=0 |
| Brew phase `FLOW` only | brew-flow `HEATER_KP_BREW`/`HEATER_KI_BREW` + `HEATER_FF_BREW_PCT` | 20.0 / 0.20 (4×) + FF 20 % |
| `STEAM_FLOW` | steam-flow `HEATER_KP_STEAM`/`HEATER_KI_STEAM` + `HEATER_FF_STEAM_PCT` | 12.5 / 0.125 (2.5×) + FF 12 % |

Цілі без offset. Feed-forward — адитивний % до виходу ПІ лише під час brew-`FLOW` / `STEAM_FLOW`, потім clamp 0…100 %. Перемикання ≤ одного `CONTROL_INTERVAL_MS`. `SAFETY_CUTOFF_C` спільний.

## Button matrix (must)

| State | B1 | B2 | B3 |
| --- | --- | --- | --- |
| ESPRESSO_HEAT | ignore | ignore | ignore |
| ESPRESSO_READY | start 1 | start 2 | enterSteam |
| BREW_1 | stop | ignore | ignore |
| BREW_2 | ignore | stop | ignore |
| STEAM_HEAT | ignore | ignore | cancel |
| STEAM_READY | ignore | ignore | cancel (not SS1) |
| STEAM_FLOW | ignore | ignore | ignore |
| STEAM_COOL | ignore | ignore | enterSteam |
| FAULT | ignore | ignore | ignore |

## Pump authority

| Context | Pump % |
| --- | --- |
| PREINFUSE | `PREINFUSION_PCT` |
| PAUSE / idle / heat / ready / cool | 0 |
| FLOW | pot `A0` (live) |
| STEAM_FLOW ON phase | `STEAM_PULSE_PCT` |
| STEAM_FLOW OFF phase | 0 |
| STEAM_FLOW even if `Tt < target` | pulse continues (no forced 0 from temp) |

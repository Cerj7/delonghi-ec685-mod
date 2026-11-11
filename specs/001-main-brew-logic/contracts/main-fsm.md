# Contract: Main Board FSM

Стани та обов’язкові дії (актуатори + кадри панелі). Числа — через константи з [tunable-constants.md](./tunable-constants.md).

## States

```
POWER_ON
   │ HE1, target = espresso pot
   ▼
ESPRESSO_HEAT ──ready(±READY_BAND_C, READY_HOLD_MS)──► ESPRESSO_READY (HE0)
   ▲                                                      │
   │                                                      ├─ B1 → BREW_1 (P11)
   │                                                      ├─ B2 → BREW_2 (P21)
   │                                                      └─ B3 → STEAM_HEAT (HS1, target=STEAM_TARGET_C)
   │
BREW_1 / BREW_2
   phases: PREINFUSE → PAUSE → FLOW
   stop: same button → P10/P20 → ESPRESSO_READY
   ignore: other B*, steam button

STEAM_HEAT (HS1)
   ├─ |Tt−STEAM_TARGET_C|≤STEAM_READY_BAND_C → STEAM_READY (HS0)  // no READY_HOLD_MS
   └─ B3 → cancel → coolOrReady()

STEAM_READY (HS0)
   ├─ handle open → STEAM_FLOW (SS1)
   ├─ idle STEAM_IDLE_EXIT_MS (>0), handle closed → STEAM_COOL (LT1)
   │     // STEAM_IDLE_EXIT_MS==0 → без авто-LT1; чекати ручку / B3
   └─ B3 → cancel → coolOrReady()   // NOT SS1
   ignore B1/B2

STEAM_FLOW
   pulse pump STEAM_PULSE_ON_MS / OFF @ STEAM_PULSE_PCT
   handle close → SS0 → coolOrReady()   // NOT back to idle HS0 / 3 s timer
   ignore B1/B2; B3 ignore during SS1

STEAM_COOL (LT1)
   target = espresso pot
   ready criterion → LT0 → ESPRESSO_READY
   ignore B1/B2

FAULT
   heater OFF, pump 0, OLED fault
   exit v1: power cycle only
```

## Helper: `coolOrReady()`

Після скасування пару (`B3` у `HS1`/`HS0`) або виходу з подачі:

1. `activeTargetC = espressoTargetC`
2. Якщо `Tt > espressoTargetC` → надіслати `LT1` (якщо ще не в cool), увійти в `STEAM_COOL`
3. Інакше → надіслати `LT0` (зняти індикацію пару); якщо `Tt` ще нижче уставки → `HE1` / `ESPRESSO_HEAT`, інакше `ESPRESSO_READY`

## Ready criterion

`|Tt - espressoTargetC| ≤ READY_BAND_C` continuously for `READY_HOLD_MS`  
(Використовується для `HE0` і для `LT0`.)

## Steam ready (`HS0`)

Щойно `|Tt − STEAM_TARGET_C| ≤ STEAM_READY_BAND_C` — одразу `HS0`. **Без** `READY_HOLD_MS`. (Опційний idle `STEAM_IDLE_EXIT_MS` лише з активного `STEAM_READY` + закрита ручка; дефолт 0 = вимкнено.)

## Button matrix (must)

| State | B1 | B2 | B3 |
| --- | --- | --- | --- |
| ESPRESSO_HEAT | ignore | ignore | ignore |
| ESPRESSO_READY | start 1 | start 2 | start steam |
| BREW_1 | stop | ignore | ignore |
| BREW_2 | ignore | stop | ignore |
| STEAM_HEAT | ignore | ignore | cancel |
| STEAM_READY | ignore | ignore | cancel (not SS1) |
| STEAM_FLOW | ignore | ignore | ignore |
| STEAM_COOL | ignore | ignore | ignore |
| FAULT | ignore | ignore | ignore |

## Pump authority

| Context | Pump % |
| --- | --- |
| PREINFUSE | `PREINFUSION_PCT` |
| PAUSE / idle / heat / ready / cool | 0 |
| FLOW | pot `A0` (live) |
| STEAM_FLOW ON phase | `STEAM_PULSE_PCT` |
| STEAM_FLOW OFF phase | 0 |

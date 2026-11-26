# Contract: Descale FSM

Розширення FSM основної плати (`scripts/013_main_brew_logic.cpp`). Базова матриця brew/steam: [../../001-main-brew-logic/contracts/main-fsm.md](../../001-main-brew-logic/contracts/main-fsm.md).

## States

| State | Entry actions | Invariants |
| --- | --- | --- |
| `DESCALE_HOLD` | Запам’ятати `pressMs`; опційно приглушити звичайну обробку short-`B3` | Помпа 0 %; рекомендовано ТЕН 0 % |
| `DESCALE_PREP` | OLED prep; reset session flags | ТЕН 0 %; помпа 0 % |
| `DESCALE_PHASE1` | `phasePulses=0` (або resume після refill — **не** нулити); старт pulse ON | ТЕН 0 %; імпульси phase1 @ `DESCALE_PUMP_PCT` |
| `DESCALE_REFILL` | Помпа 0; OLED refill; **зберегти** `phasePulses` | ТЕН 0 % |
| `DESCALE_WAIT_RINSE` | Помпа 0; OLED rinse prompt; `DC1` **не** скидати | ТЕН 0 % |
| `DESCALE_PHASE2` | `phasePulses=0`; старт pulse ON | ТЕН 0 %; імпульси phase2 |

## Transitions

| From | Event | To | Notes |
| --- | --- | --- | --- |
| `ESPRESSO_READY` / `ESPRESSO_HEAT` | `B3` press | `DESCALE_HOLD` | Не в brew/steam/`FAULT` |
| `DESCALE_HOLD` | elapsed ≥ `DESCALE_HOLD_ENTER_MS` | `DESCALE_PREP` | Незалежно від `dcLatched` |
| `DESCALE_HOLD` | `R3` і elapsed < `DESCALE_HOLD_PROMPT_MS` | попередній / steam path | Short press → `enterSteam()` з READY; з HEAT — за матрицею `001` (ігнор або deferred) |
| `DESCALE_HOLD` | `R3` і prompt ≤ elapsed < enter | `ESPRESSO_READY` / `ESPRESSO_HEAT` | Cancel countdown; без пару |
| `DESCALE_PREP` | `B3` + handle open | `DESCALE_PHASE1` | |
| `DESCALE_PREP` | `B3` + handle closed | stay | OLED handle closed |
| `DESCALE_PHASE1` | no-flow ON ≥ P1 timeout **і** vol < target | `DESCALE_REFILL` | |
| `DESCALE_PHASE1` | vol ≥ target (або no-flow при vol ≥ target) | `DESCALE_WAIT_RINSE` | Пріоритет над refill |
| `DESCALE_REFILL` | `B3` + handle open | `DESCALE_PHASE1` | Resume pulses; volume kept |
| `DESCALE_REFILL` | `B3` + handle closed | stay | OLED handle closed |
| `DESCALE_WAIT_RINSE` | `B3` + handle open | `DESCALE_PHASE2` | |
| `DESCALE_WAIT_RINSE` | `B3` + handle closed | stay | OLED handle closed |
| `DESCALE_PHASE2` | vol ≥ target **або** no-flow ON ≥ P2 timeout | success exit | `DC0` effect → espresso path |
| Any `DESCALE_*` | fault / invalid thermocouple / cutoff | `FAULT` | ТЕН/помпа OFF; **без** `DC0` |
| Any `DESCALE_*` | power loss | (cold start) | Програма не resume; latch як був |

## Button matrix (delta)

| State | `B1`/`B2` | `B3` | `R3` | Handle close mid-phase |
| --- | --- | --- | --- | --- |
| `DESCALE_HOLD` | ignore | (already held) | cancel / short steam | n/a |
| `DESCALE_PREP` | ignore | start P1 if open | ignore | n/a |
| `DESCALE_PHASE1` | ignore | ignore | ignore | continue pulses |
| `DESCALE_REFILL` | ignore | resume P1 if open | ignore | n/a |
| `DESCALE_WAIT_RINSE` | ignore | start P2 if open | ignore | n/a |
| `DESCALE_PHASE2` | ignore | ignore | ignore | continue pulses |

## Pump / heater gates

- `heaterForceOff` == true для всіх станів від `DESCALE_PREP`…`DESCALE_PHASE2` (і рекомендовано `DESCALE_HOLD`).
- `pumpMayRun` == true лише в `DESCALE_PHASE1` і `DESCALE_PHASE2` під час pulse ON.
- Не надсилати `HS*`/`SS*`/`P1*`/`P2*` під час `DESCALE_*` (крім можливо нейтралізації попередніх LED при вході — імплементація MAY послати `SS0`/`HS0`/`LT0` один раз при вході в prep, якщо потрібно зняти «паровий» LED; не обов’язково для FR).

## Success exit sequence

1. Помпа 0 %.
2. Виклик спільного clear: `sendToPanel("DC0")` + `dcLatched=false` + `accumulatedOnSeconds=0` + EEPROM save (як USB `DC0`).
3. Перехід у `ESPRESSO_READY` або `ESPRESSO_HEAT` за температурою/уставкою еспресо (без парової цілі).

## Timing tolerances (test)

| Parameter | Tolerance |
| --- | --- |
| Hold enter 10 s | ±200 мс (SC-001) |
| Phase pulses | ±50 мс (SC-003) |
| `DC0` after phase 2 complete | ≤1 с (SC-005) |

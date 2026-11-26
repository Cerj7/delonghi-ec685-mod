# Contract: Descale Tunable Constants

Усі числові параметри програми декальцинації SHALL бути іменованими константами в секції Tunables на початку `scripts/013_main_brew_logic.cpp` (поруч з блоком Decalc / Timing). У тілі FSM/loop заборонені «магічні» літерали бізнес-логіки (FR-016, SC-007).

Базові константи brew/steam/decalc-reminder: [../../001-main-brew-logic/contracts/tunable-constants.md](../../001-main-brew-logic/contracts/tunable-constants.md).

## Hold / enter

| Constant | Default | Used for |
| --- | --- | --- |
| `DESCALE_HOLD_PROMPT_MS` | 1000 | Після цього утримання показувати OLED-відлік |
| `DESCALE_HOLD_ENTER_MS` | 10000 | Безперервне утримання → `DESCALE_PREP` |
| `DESCALE_HOLD_ENTER_TOL_MS` | 200 | Допуск стенду SC-001 (тест, не обов’язково в логіці) |

## Volume / flow

| Constant | Default | Used for |
| --- | --- | --- |
| `DESCALE_PHASE_VOLUME_ML` | 1000 | Ціль об’єму фази 1 і фази 2 |
| `FLOW_PULSES_PER_ML` | (з `001`, MUST > 0 для прод) | Перетворення імпульсів → мл |

Стенд MAY тимчасово зменшити `DESCALE_PHASE_VOLUME_ML` або підставити калібрований `FLOW_PULSES_PER_ML`; обидва лишаються Tunables.

## Phase 1 (clean)

| Constant | Default | Used for |
| --- | --- | --- |
| `DESCALE_PHASE1_ON_MS` | 500 | Імпульс помпи ON |
| `DESCALE_PHASE1_OFF_MS` | 1000 | Пауза відмочування |
| `DESCALE_PHASE1_NOFLOW_MS` | 1000 | No-flow під час ON → refill або complete |

## Phase 2 (rinse)

| Constant | Default | Used for |
| --- | --- | --- |
| `DESCALE_PHASE2_ON_MS` | 500 | Імпульс помпи ON |
| `DESCALE_PHASE2_OFF_MS` | 500 | Пауза між імпульсами |
| `DESCALE_PHASE2_NOFLOW_MS` | 1500 | No-flow під час ON → завершення фази 2 |

## Heater

| Constant | Default | Used for |
| --- | --- | --- |
| `DESCALE_PHASE1_TEMP_C` | 53 | Уставка ТЕНа **лише** в `DESCALE_PHASE1` (FR-006; штатний PIC ≈ ADC `0x0177`) |

Prep / refill / wait-rinse / phase 2 / refill water: ТЕН force-off (0 %), окремий Tunable уставки не потрібен. Гістерезис — reuse PI з `001` (окремий band MAY, не обов’язковий у v1).

## Power

| Constant | Default | Used for |
| --- | --- | --- |
| `DESCALE_PUMP_PCT` | 100 | Потужність під час ON (не потенціометр) |
| `PUMP_OFF_PCT` | 0 | Явний стоп (reuse `001`) |
| `HEATER_OFF_PCT` | 0 | Force-off поза фазою 1 (reuse / alias `001` якщо вже є) |

## Timing tolerances (test aids)

| Constant | Default | Used for |
| --- | --- | --- |
| `DESCALE_PULSE_TOL_MS` | 50 | Допуск SC-003 |
| `DESCALE_DC0_MAX_LATENCY_MS` | 1000 | Допуск SC-005 |

## Compliance check

Перед merge:

1. `rg` по `013`: немає літералів `10000`, `1000`, `500`, `1500`, `53` у логіці descale поза Tunables (окрім коментарів / таблиць калібрування / посилань на docs).
2. Немає використання `STEAM_PULSE_*` / еспресо-уставки всередині descale-фаз (фаза 1 бере лише `DESCALE_PHASE1_TEMP_C`).
3. `FLOW_PULSES_PER_ML > 0` задокументовано в quickstart як prerequisite для об’ємних сценаріїв.
4. Heater gate: 53 °C тільки в `DESCALE_PHASE1`; усі інші `DESCALE_*` (крім hold-countdown) — 0 %.

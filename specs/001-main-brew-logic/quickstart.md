# Quickstart: Main Brew Logic validation

Перевірка end-to-end після збірки `scripts/013_main_brew_logic.cpp`. Контракти: [panel-uart.md](./contracts/panel-uart.md), [main-fsm.md](./contracts/main-fsm.md), константи: [tunable-constants.md](./contracts/tunable-constants.md).

## Prerequisites

1. Arduino IDE + бібліотеки з `readme.md` (GFX, SSD1306, MAX6675, AC_Dimmer).
2. Nano панелі з `scripts/011_button_panel_ui.cpp`.
3. Основна Nano з `013_main_brew_logic.cpp` (після імплементації).
4. З’єднання: `D6↔D9`, `D5↔D8`, `GND↔GND`; термоблок/помпа/OLED/ручка/`D5` флоуметр за pinout.
5. USB Serial основної: **115200**.

## Setup

1. Залити панель (`011`), потім основну (`013`).
2. Відкрити Serial Monitor основної.
3. Для швидкого тесту DC тимчасово зменшити `DECALC_INTERVAL_S` у Tunables **або** використати сервісні команди (якщо реалізовані).

## Scenarios

### S1 — Розігрів (P1)

1. Подати живлення.
2. Очікувано: ≤1 с кадр `HE1`, LED1+LED2 мигають; `B1`/`B2`/`B3` не вмикають помпу.
3. Коли Tt у `READY_BAND_C` протягом `READY_HOLD_MS` → `HE0`, LED постійні.

### S2 — 1 еспресо (P1)

1. З готовності натиснути «1 еспресо» → `B1` → `P11`.
2. Перевірити: `PREINFUSION_MS` @ `PREINFUSION_PCT` → пауза `PREINFUSION_PAUSE_MS` → пролив за потенціометром (±`TIMING_TOLERANCE_MS`).
3. OLED ≥ кожні `BREW_DISPLAY_INTERVAL_MS`: лічильник, Tt, % помпи.
4. Повторне «1» → помпа 0 за ≤`BUTTON_REACT_MAX_MS`, `P10`.
5. Під час проливу «2»/«пар» не змінюють стан.

### S3 — 2 еспресо (P1)

Аналогічно S2 з `B2` / `P21` / `P20`.

### S4 — Пар (P2)

1. З готовності «пар» → `HS1`, ціль `STEAM_TARGET_C`.
2. Повторне «пар» до цілі → cancel + `coolOrReady()` (`LT1`… або `LT0`).
3. `|Tt − STEAM_TARGET_C| ≤ STEAM_READY_BAND_C` → одразу `HS0` (без hold ≥1 с).
4. Відкрити ручку → `SS1`, OLED `OPEN`, імпульси ON/OFF; закрити → `SS0`, помпа 0 ≤300 мс, далі `coolOrReady()` (не повернення в idle `HS0`).
5. У `HS0` без ручки з `STEAM_IDLE_EXIT_MS=0` машина лишається в готовності; повторне `B3` → cancel; **не** `SS1`.
6. (Опційно) якщо Tunable idle > 0 — без ручки стільки мс → `LT1`…`LT0`.
7. Під час HS/SS/LT — `B1`/`B2` ігнор.

### S5 — Декальцинація (P3)

1. Симулювати кінець **uptime**-інтервалу (зменшена `DECALC_INTERVAL_S` / force) → один `DC1`.
2. Вимкнути живлення посередині → після увімкнення `accumulatedOnSeconds` не з нуля.
3. USB команда `DC0` → кадр `DC0` на панель, акумулятор обнулено.

### S6 — Аварія

1. Від’єднати CS/симулювати невалідну Tt → ТЕН OFF, помпа 0, OLED fault; активний пролив/пар зупиняється; `FAULT` тримається до power cycle.

## Pass criteria

Усі acceptance scenarios зі [spec.md](./spec.md) (US1–US5) і SC-001…SC-008 на стенді. Тюнінг лише через секцію констант без правок FSM.

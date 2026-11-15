# Quickstart: Main Brew Logic validation

Перевірка end-to-end після збірки `scripts/013_main_brew_logic.cpp`. Контракти: [panel-uart.md](./contracts/panel-uart.md), [main-fsm.md](./contracts/main-fsm.md), константи: [tunable-constants.md](./contracts/tunable-constants.md).

## Prerequisites

1. Arduino IDE + бібліотеки з `readme.md` (GFX, SSD1306, MAX6675, AC_Dimmer).
2. Nano панелі з `scripts/011_button_panel_ui.cpp` (**без змін**).
3. Основна Nano з `013_main_brew_logic.cpp` (після імплементації session 4).
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
3. ТЕН на **heat-up** профілі; допустимий короткий overshoot ~+3…5 °C над уставкою (нижче cutoff).
4. Коли `Tt ≥ target − READY_BAND_C` протягом `READY_HOLD_MS` → `HE0`, LED постійні (верхньої межі немає; overshoot не блокує).

### S2 — Холодний пролив / `B1` (P1)

1. З готовності натиснути «1 еспресо» → `B1` → `P11`.
2. Перевірити: помпа `COLD_FLUSH_MS` @ `COLD_FLUSH_PCT` (4 с @ 100 %, ±`TIMING_TOLERANCE_MS`); **ТЕН 0 %**.
3. OLED ≥ кожні `BREW_DISPLAY_INTERVAL_MS`: лічильник, Tt, % помпи.
4. Повторне «1» до таймауту → помпа 0 за ≤`BUTTON_REACT_MAX_MS`, `P10`.
5. Без повторного «1» → після 4 с `P10`, готовність.
6. Під час flush «2»/«пар» не змінюють стан.
7. Під час `P21` / `HS1` / `SS1` / `LT1` cold flush **не** стартує.

### S3 — 2 еспресо (P1)

1. З готовності «2 еспресо» → `B2` → `P21`.
2. `PREINFUSION_MS` @ `PREINFUSION_PCT` → пауза → пролив за потенціометром (±`TIMING_TOLERANCE_MS`).
3. Преінфузія/пауза — base ПІ; `FLOW` — brew-flow (~5× + FF); після `P20` — base.
4. Повторне «2» → `P20` ≤`BUTTON_REACT_MAX_MS`; `B1`/`B3` ігнор.

### S4 — Пар (P2)

1. З готовності «пар» → якщо `Tt < STEAM_TARGET_C` то `HS1` (heat-up ПІ), інакше одразу `HS0`; ціль `STEAM_TARGET_C`.
2. Повторне «пар» під час `HS1`/`HS0`/`SS*` → **ігнор** (не cancel).
3. `Tt ≥ STEAM_TARGET_C` (у т.ч. Tt > 122 °C) → одразу `HS0` (без hold ≥1 с).
4. Після `HS0` ціль 120 °C утримується; при закритій ручці падіння Tt → авто `HS1`→`HS0`.
5. Відкрити ручку → `SS1`, OLED `OPEN`, імпульси + **steam-flow** ПІ; закрити → `SS0` → **назад у `HS0`** (не cool).
6. **Немає** idle→`LT1`: чекати довільний час із закритою ручкою без `B1`/`B2` — `LT1` не з’являється.
7. Натиснути `B1` або `B2` у `HS1`/`HS0`/`SS*` → `LT1` (LED3 double-blink за протоколом); підтримка пару припиняється; ТЕН OFF.
8. У `LT1` відкрити ручку → імпульси 110/880 @ 100 %; при готовності еспресо → помпа 0 навіть якщо ручка відкрита → `LT0`.
9. Під час `LT1`…`LT0` «пар» (`B3`) → знову `HS0`/`HS1`.
10. Під час **`SS1`/`STEAM_FLOW`** Tt < target → імпульси продовжуються, без `HS1`/`SS0` (FR-026).

### S5 — Декальцинація (P3)

1. Симулювати кінець **uptime**-інтервалу (зменшена `DECALC_INTERVAL_S` / force) → один `DC1`.
2. Вимкнути живлення посередині → після увімкнення `accumulatedOnSeconds` не з нуля.
3. USB команда `DC0` → кадр `DC0` на панель, акумулятор обнулено.

### S6 — Аварія

1. Від’єднати CS/симулювати невалідну Tt → ТЕН OFF, помпа 0, OLED fault; активний пролив/пар зупиняється; `FAULT` тримається до power cycle.

### S7 — Heat-up (стенд)

1. З холодної машини порівняти час до `HE0` / `HS0` з heat-up vs лише base (якщо тимчасово зрівняти Kp).
2. Overshoot не повинен тригерити `SAFETY_CUTOFF_C` / `FAULT`; орієнтир ≤`HEAT_UP_OVERSHOOT_OK_C`.

## Pass criteria

Усі acceptance scenarios зі [spec.md](./spec.md) (US1–US5) і SC-001…SC-012 на стенді. Тюнінг лише через секцію констант без правок FSM.

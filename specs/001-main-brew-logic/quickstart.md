# Quickstart: Main Brew Logic validation

Перевірка end-to-end після збірки `scripts/013_main_brew_logic.cpp`. Контракти: [panel-uart.md](./contracts/panel-uart.md), [main-fsm.md](./contracts/main-fsm.md), константи: [tunable-constants.md](./contracts/tunable-constants.md).

Оновлено після clarify 2026-11-15 (dual steam 120/128, pot ranges, OLED layouts).

## Prerequisites

1. Arduino IDE + бібліотеки з `readme.md` (GFX, SSD1306, MAX6675, AC_Dimmer).
2. Nano панелі з `scripts/011_button_panel_ui.cpp` (**без змін**).
3. Основна Nano з `013_main_brew_logic.cpp` (після імплементації delta 2026-11-15).
4. З’єднання: `D6↔D9`, `D5↔D8`, `GND↔GND`; термоблок/помпа/OLED/ручка/`D5` флоуметр за pinout.
5. USB Serial основної: **115200**.

## Setup

1. Залити панель (`011`), потім основну (`013`).
2. Відкрити Serial Monitor основної.
3. Для швидкого тесту DC тимчасово зменшити `DECALC_INTERVAL_S` у Tunables **або** використати сервісні команди (якщо реалізовані).
4. Переконатись у Tunables: `TARGET_MIN/MAX_C=85/105`, `PUMP_MIN/MAX_PCT=50/100`, `STEAM_READY_TARGET_C=120`, `STEAM_FLOW_TARGET_C=128`, `SAFETY_CUTOFF_C > 128`. **Не** міняти стендові ПІ Kp/Ki/FF «під таблицю».

## Scenarios

### S1 — Розігрів (P1)

1. Подати живлення.
2. Очікувано: ≤1 с кадр `HE1`, LED1+LED2 мигають; `B1`/`B2`/`B3` не вмикають помпу.
3. ТЕН на **heat-up** профілі; уставка pot у діапазоні 85…105 °C.
4. Коли `Tt ≥ target − READY_BAND_C` протягом `READY_HOLD_MS` → `HE0` (верхньої межі немає).

### S2 — Холодний пролив / `B1` (P1)

1. З готовності натиснути «1 еспресо» → `B1` → `P11`.
2. Перевірити: помпа `COLD_FLUSH_MS` @ `COLD_FLUSH_PCT`; **ТЕН 0 %**.
3. OLED: зворотний відлік **`Cold flush N s`** (`N` зменшується; ±1 с) — FR-033 / SC-016.
4. Повторне «1» → помпа 0 ≤`BUTTON_REACT_MAX_MS`, `P10`.
5. Без повторного «1» → після таймауту `P10`, готовність.
6. Під час flush «2»/«пар» не змінюють стан.
7. Під час `P21` / `HS1` / `SS1` / `LT1` cold flush **не** стартує.

### S3 — 2 еспресо (P1)

1. З готовності «2 еспресо» → `B2` → `P21`.
2. Преінфузія → пауза → пролив; потужність FLOW у діапазоні **50…100 %** (pot A0).
3. Преінфузія/пауза — base ПІ; `FLOW` — brew-flow + FF (значення зі стенду).
4. OLED ≥ кожні `BREW_DISPLAY_INTERVAL_MS`: **Tt / уставка / ТЕН% / помпа%** (FR-008 / SC-004).
5. Повторне «2» → `P20` ≤`BUTTON_REACT_MAX_MS`; `B1`/`B3` ігнор.

### S4 — Пар (P2)

1. З готовності «пар» → якщо `Tt < 120` то `HS1` (ціль 120), інакше одразу `HS0`.
2. Повторне «пар» під час `HS1`/`HS0`/`SS*` → **ігнор**.
3. `Tt ≥ 120` → одразу `HS0` (без hold).
4. Після `HS0` (ручка закрита) утримання **120 °C**; падіння → авто `HS1`→`HS0`.
5. Відкрити ручку → `SS1`, ціль **128 °C**, OLED `open`, імпульси + steam-flow; закрити → `SS0`, ціль знову **120**, `HS0`.
6. OLED під час пару: **Tt / ТЕН% / open|close** (FR-029).
7. **Немає** idle→`LT1`.
8. `B1` або `B2` у `HS*`/`SS*` → `LT1`; ТЕН OFF; OLED лишає steam-layout (ТЕН%=0, open|close).
9. У `LT1` відкрити ручку → імпульси; при готовності еспресо → помпа 0 → `LT0`.
10. Під час `LT1`…`LT0` `B3` → знову `HS0`/`HS1`.
11. Під час `SS1` Tt < 128 (або < 120) → імпульси продовжуються, без `HS1`/`SS0`.

### S5 — Декальцинація (P3)

1. Симулювати кінець uptime-інтервалу → один `DC1`.
2. Вимкнути живлення посередині → акумулятор не з нуля.
3. USB `DC0` → кадр `DC0`, акумулятор обнулено.

### S6 — Аварія

1. Невалідна Tt → ТЕН OFF, помпа 0, OLED fault; `FAULT` до power cycle.
2. Переконатись, що робота на 128 °C **не** тригерить `SAFETY_CUTOFF_C`.

### S7 — Heat-up / pot ranges (стенд)

1. Heat-up vs base: час до `HE0`/`HS0` коротший із heat-up; overshoot < cutoff.
2. Край pot температури ≈ 85 і 105 °C; край pot помпи ≈ 50 і 100 %.

## Pass criteria

Усі acceptance scenarios зі [spec.md](./spec.md) (US1–US5) і SC-001…SC-016 на стенді. Тюнінг лише через секцію констант; ПІ цієї ітерації не відкочувати «під таблицю».

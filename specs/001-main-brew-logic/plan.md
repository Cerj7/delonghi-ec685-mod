# Implementation Plan: Main Brew Logic (EC685)

**Branch**: `001-main-brew-logic` | **Date**: 2026-11-15 (plan refresh after clarify session 2026-11-15) | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/001-main-brew-logic/spec.md`

**Note**: This template is filled in by the `/speckit-plan` command; its definition describes the execution workflow.

## Summary

Нова прошивка основної Arduino Nano, що відтворює штатну поведінку EC685: розігрів еспресо → готовність → **холодний пролив (`B1`)** / **еспресо з преінфузією (`B2`)** → пар → охолодження (`LT1`, опційно з імпульсною помпою від ручки) → нагадування декальцинації раз на 28 діб. Технічний підхід: об’єднати ПІ-нагрів і диммер з `006_thermoblock_pi_heater.cpp`, UART-контракт панелі з `docs/button-panel-protocol.md` / `012_main_panel_link.cpp`, плюс FSM режимів, флоуметр, ручку пари та EEPROM-таймер `DC1`/`DC0`. Усі значущі числові параметри виносяться в Tunables на початку скетчу. **Прошивка панелі `011` не змінюється.**

**Пар (sessions 2–4, оновлено 2026-11-15)**:
- Дві цілі: `STEAM_READY_TARGET_C = 120` (нагрів / `HS0`, ручка закрита); `STEAM_FLOW_TARGET_C = 128` (ручка відкрита / `STEAM_FLOW`).
- Готовність `HS0`: `Tt ≥ STEAM_READY_TARGET_C` (без hold / без верхньої смуги).
- У `STEAM_READY` (ручка закрита): падіння нижче 120 → авто `HS1`→`HS0`; відкриття ручки → `SS1` + ціль 128.
- У `STEAM_FLOW`: ціль 128; `Tt < 128` (або `< 120`) **не** дає `HS1`/`SS0` і **не** паузить імпульси (FR-026).
- Закриття ручки: `SS0` → ціль знову 120 → `HS0` (не миттєвий cool).
- Вихід з пару: лише `B1`/`B2` → `LT1`; idle-автовихід вимкнено; `B3` у `HS*`/`SS*` ігнорується; `B3`@`STEAM_COOL` → `enterSteam()`.

**Clarify 2026-11-15 — delta**:
1. **Не відкочувати** стендові ПІ Kp/Ki/FF у `013` (source of truth = Tunables у прошивці).
2. **Діапазони pot**: температура `TARGET_MIN_C`…`TARGET_MAX_C` = **85…105 °C**; помпа `PUMP_MIN_PCT`…`PUMP_MAX_PCT` = **50…100 %**.
3. **OLED `BREW_2`**: Tt / уставка / ТЕН% / помпа% (FR-008) — без обов’язкового лічильника води.
4. **OLED cold flush**: зворотний відлік «Cold flush N s» (EN fallback; UA якщо шрифт дозволяє) (FR-033).
5. **OLED пар + `STEAM_COOL`**: Tt / ТЕН% / `open`|`close` (FR-029).

## Technical Context

**Language/Version**: C++ (Arduino AVR toolchain / Arduino IDE), ATmega328P

**Primary Dependencies**: Adafruit GFX, Adafruit SSD1306, Adafruit MAX6675, AC_dimmer-dmr-0.2.1, SoftwareSerial, avr/wdt, EEPROM

**Storage**: EEPROM (накопичений uptime декальцинації: `accumulatedOnSeconds` + прапорець `dcLatched` / magic; інтервал 28×86400 с роботи)

**Testing**: Ручний стенд (дві Nano: панель `011` + основна нова прошивка); USB Serial 115200 для логів і сервісної команди `DC0`; сценарії з [quickstart.md](./quickstart.md)

**Target Platform**: Arduino Nano v3 (ATmega328P), кавоварка DeLonghi EC685 (мод)

**Project Type**: Embedded firmware (single sketch + docs)

**Performance Goals**: Відповідь на кнопку ≤300 мс; OLED під час brew/flush/steam ≥2 Гц (`BREW_DISPLAY_INTERVAL_MS` / `OLED_INTERVAL_MS`); `HE0`: `Tt ≥ target − 1 °C` / ≥1.0 с (без верхньої межі); `LT0`: `Tt ≤ target + 1 °C` / ≥1.0 с; пар-готовність `Tt ≥ STEAM_READY_TARGET_C` миттєво; таймінги cold flush / преінфузії / імпульсів ±200 мс; перемикання ПІ-профілю ≤ одного `CONTROL_INTERVAL_MS`

**Constraints**: Flash/RAM Nano; SoftwareSerial 19200 на D8/D9; `SAFETY_CUTOFF_C` строго вище `STEAM_FLOW_TARGET_C` (128 °C) і heat-up overshoot; помпа 0 % поза cold flush / brew-2 / паровими імпульсами (включно з `STEAM_COOL`+ручка); watchdog ~2 с; панель `011` без змін; SSD1306 default font → cold-flush текст EN («Cold flush N s»), доки немає кириличного шрифту

**Scale/Scope**: 1 нова прошивка основної плати + docs; панель `011` без змін контракту; ~1 FSM з 9 станами; **4** ПІ-профілі; **2** парові температурні константи

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Конституція: `.specify/memory/constitution.md` **v1.0.0** (ratified 2026-11-11).

| Gate | Status | Notes |
| --- | --- | --- |
| I. Hardware Safety | PASS | Невалідна термопара / `SAFETY_CUTOFF_C` (>128 °C) → ТЕН OFF, помпа 0, `FAULT` до power cycle; cold flush і `STEAM_COOL` — ТЕН OFF |
| II. Memory Efficiency | PASS | Немає `malloc`/`new` у loop; EEPROM буфер фіксованого розміру; статичні буфери UART / OLED рядків |
| III. Deterministic Real-Time | PASS | FSM + `millis()`; без блокуючих `delay()` у керуванні; ISR лише короткий лічильник імпульсів |
| IV. Maintainable & Modular | EXCEPTION | Монолітний `013` зі секціями (як `006`/`012`); див. Complexity Tracking |
| Контракт панелі / Tunables / YAGNI DC UI | PASS | Коди з protocol без зміни `011`; усі °C/мс/%/Kp/Ki у Tunables; UI декальцинації out of scope |

**Post-design re-check** (2026-11-15): PASS з задокументованим винятком IV. Dual steam targets / pot ranges / OLED layouts — лише Tunables + display path + `activeTargetC` selection; cutoff лишається вище 128 °C; панель без змін.

### Implementation delta (після plan refresh 2026-11-15)

Код `scripts/013_main_brew_logic.cpp` частково вже має `TARGET_MIN/MAX=85/105` і стендові ПІ; ще потрібні зміни під нову spec. Після цього plan — синхронізувати Tunables + FSM + OLED за контрактами, потім `/speckit-tasks` (delta) або implement.

Ключові code deltas:
1. Замінити єдиний `STEAM_TARGET_C` на `STEAM_READY_TARGET_C=120` + `STEAM_FLOW_TARGET_C=128`; `HS0` / auto-`HS1` на 120; у `STEAM_FLOW` / handle-open — `activeTargetC=128`; після `SS0` — знову 120.
2. Додати `PUMP_MIN_PCT=50` / `PUMP_MAX_PCT=100`; мапінг A0 на цей діапазон (як A1 → `TARGET_MIN/MAX`).
3. Підтвердити `TARGET_MIN_C=85` / `TARGET_MAX_C=105` у Tunables + контракті (не відкочувати).
4. **Не змінювати** поточні `HEATER_KP_*` / `HEATER_KI_*` / `HEATER_FF_*` у прошивці.
5. OLED `BREW_2`: Tt / setpoint / heater% / pump% (без обов’язкового water counter).
6. OLED `COLD_FLUSH`: countdown «Cold flush N s» (`N=ceil(remainingMs/1000)`).
7. OLED steam + `STEAM_COOL`: Tt / heater% / `open`|`close`.
8. Синхронізувати `docs/main-brew-logic.md` і quickstart після коду.

## Project Structure

### Documentation (this feature)

```text
specs/001-main-brew-logic/
├── plan.md              # This file
├── research.md          # Phase 0
├── data-model.md        # Phase 1
├── quickstart.md        # Phase 1
├── contracts/           # Phase 1
│   ├── panel-uart.md
│   ├── main-fsm.md
│   └── tunable-constants.md
└── tasks.md             # потребує delta після цього plan
```

### Source Code (repository root)

```text
scripts/
├── 006_thermoblock_pi_heater.cpp   # база: ПІ, диммер, OLED, захист
├── 011_button_panel_ui.cpp         # панель (без змін у цій фічі)
├── 012_main_panel_link.cpp         # заглушка UART (референс лінку)
└── 013_main_brew_logic.cpp         # НОВА робоча прошивка основної плати

docs/
├── button-panel-protocol.md        # UART-контракт (вже є; LED LT1 без змін)
├── thermoblock-pi-heater.md        # референс ПІ/захисту
└── main-brew-logic.md              # опис нової прошивки (синхронізувати з 2026-11-15)

specs/001-main-brew-logic/          # plan / research / model / contracts / quickstart
```

**Structure Decision**: Один новий скетч `scripts/013_main_brew_logic.cpp` у стилі існуючих стендів (монолітний `.cpp` з секціями констант / пінів / FSM / loop), плюс feature-docs у `specs/001-main-brew-logic/` і згодом `docs/main-brew-logic.md`. Окремі бібліотеки/пакети не виділяємо — обмеження flash/RAM і узгодженість з `006`/`012`.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
| --- | --- | --- |
| Constitution IV: монолітний `013_main_brew_logic.cpp` замість окремих C++ класів/модулів на драйвер | Узгодженість зі стендами `006`/`012`, ліміт Flash/RAM Nano, один артефакт прошивки основної плати | Окремі `.h`/класи на кожен драйвер збільшують обв’язку без виграшу для однієї плати; логічні секції + Tunables дають підтримуваність у межах фічі |

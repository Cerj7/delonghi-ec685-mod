# Implementation Plan: Main Brew Logic (EC685)

**Branch**: `001-main-brew-logic` | **Date**: 2026-11-12 (plan refresh after clarify session 4) | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/001-main-brew-logic/spec.md`

**Note**: This template is filled in by the `/speckit-plan` command; its definition describes the execution workflow.

## Summary

Нова прошивка основної Arduino Nano, що відтворює штатну поведінку EC685: розігрів еспресо → готовність → **холодний пролив (`B1`)** / **еспресо з преінфузією (`B2`)** → пар → охолодження (`LT1`, опційно з імпульсною помпою від ручки) → нагадування декальцинації раз на 28 діб. Технічний підхід: об’єднати ПІ-нагрів і диммер з `006_thermoblock_pi_heater.cpp`, UART-контракт панелі з `docs/button-panel-protocol.md` / `012_main_panel_link.cpp`, плюс FSM режимів, флоуметр, ручку пари та EEPROM-таймер `DC1`/`DC0`. Усі значущі числові параметри виносяться в Tunables на початку скетчу. **Прошивка панелі `011` не змінюється.**

**Пар (clarify 2026-11-11 session 2–3, частково superseded session 4)**:
- Готовність: `Tt ≥ STEAM_TARGET_C` (без ±2 / 118…122); overshoot OK.
- Після `HS0` утримувати ціль 120 °C; у `STEAM_READY` з закритою ручкою падіння → авто `HS1`→`HS0`; з уже відкритою ручкою після `HS0` — авто `SS1`.
- Закриття ручки: `SS0` → назад у `HS0` (не миттєвий `coolOrReady`).
- У `STEAM_FLOW`: `Tt < target` **не** дає `HS1`/`SS0` і **не** паузить імпульси (FR-026).
- `B3` під час `STEAM_COOL` → `enterSteam()` (`HS0`/`HS1`).

**Clarify 2026-11-12 session 4 — delta**:
- **Вихід з пару**: лише `B1` або `B2` у `HS1`/`HS0`/`SS1`/`SS0` → `LT1` / `coolOrReady()`. Idle-автовихід **вимкнено** (`STEAM_IDLE_EXIT_MS=0`). `B3` у `HS*`/`SS*` **ігнорується** (не cancel).
- **Індикація `LT1`**: чинний протокол (LED3 double-blink); LED1+LED2 не вимагаємо (панель без змін).
- **`B1` = холодний пролив**: `P11` → `COLD_FLUSH_MS` (4.0 с) @ 100 %, ТЕН OFF → `P10`; повторний `B1` зупиняє раніше. Блок: не стартувати під час `P21`/`HS1`/`SS1`/`LT1`.
- **`B2` = єдине еспресо** з преінфузією (`P21`/`P20`).
- **`STEAM_COOL` + ручка**: імпульси як у парі; ТЕН OFF; при критерії готовності еспресо → помпа 0 (навіть якщо ручка відкрита) → `LT0` (FR-028).
- **Heat-up ПІ**: окремий агресивніший профіль для `ESPRESSO_HEAT` / `STEAM_HEAT`; критерії `HE0`/`HS0` без змін; overshoot ~+3…5 °C OK (FR-027 → **4** профілі).

## Technical Context

**Language/Version**: C++ (Arduino AVR toolchain / Arduino IDE), ATmega328P

**Primary Dependencies**: Adafruit GFX, Adafruit SSD1306, Adafruit MAX6675, AC_dimmer-dmr-0.2.1, SoftwareSerial, avr/wdt, EEPROM

**Storage**: EEPROM (накопичений uptime декальцинації: `accumulatedOnSeconds` + прапорець `dcLatched` / magic; інтервал 28×86400 с роботи)

**Testing**: Ручний стенд (дві Nano: панель `011` + основна нова прошивка); USB Serial 115200 для логів і сервісної команди `DC0`; сценарії з [quickstart.md](./quickstart.md)

**Target Platform**: Arduino Nano v3 (ATmega328P), кавоварка DeLonghi EC685 (мод)

**Project Type**: Embedded firmware (single sketch + docs)

**Performance Goals**: Відповідь на кнопку ≤300 мс; OLED/лічильник під час проливу ≥2 Гц; `HE0`: `Tt ≥ target − 1 °C` / ≥1.0 с (без верхньої межі); `LT0`: `Tt ≤ target + 1 °C` / ≥1.0 с; пар-готовність `Tt ≥ STEAM_TARGET_C` миттєво; таймінги cold flush / преінфузії / імпульсів ±200 мс; перемикання ПІ-профілю ≤ одного `CONTROL_INTERVAL_MS`

**Constraints**: Flash/RAM Nano; SoftwareSerial 19200 на D8/D9 (короткі кадри); захисний cutoff ТЕНу ≥ роботи на 120 °C і heat-up overshoot; помпа 0 % поза cold flush / brew-2 / паровими імпульсами (включно з `STEAM_COOL`+ручка); watchdog ~2 с; агресивні профілі не знімають `SAFETY_CUTOFF_C`; панель `011` без змін

**Scale/Scope**: 1 нова прошивка основної плати + docs; панель `011` без змін контракту (лише споживач); ~1 FSM з 9 станами (`BREW_1` → `COLD_FLUSH`); **4** ПІ-профілі нагріву

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Конституція: `.specify/memory/constitution.md` **v1.0.0** (ratified 2026-11-11).

| Gate | Status | Notes |
| --- | --- | --- |
| I. Hardware Safety | PASS | Невалідна термопара / `SAFETY_CUTOFF_C` → ТЕН OFF, помпа 0, `FAULT` до power cycle; heat-up/brew/steam профілі не обходять cutoff; у cold flush і `STEAM_COOL` ТЕН примусово OFF |
| II. Memory Efficiency | PASS | Немає `malloc`/`new` у loop; EEPROM буфер фіксованого розміру; статичні буфери UART |
| III. Deterministic Real-Time | PASS | FSM + `millis()`; без блокуючих `delay()` у керуванні; ISR лише короткий лічильник імпульсів |
| IV. Maintainable & Modular | EXCEPTION | Монолітний `013` зі секціями (як `006`/`012`); див. Complexity Tracking |
| Контракт панелі / Tunables / YAGNI DC UI | PASS | Коди з protocol без зміни `011`; усі °C/мс/%/Kp/Ki у Tunables; UI декальцинації out of scope |

**Post-design re-check** (2026-11-12 session 4): PASS з задокументованим винятком IV. Session 4 delta не змінює gates: cold flush / LT1 pulse лишають cutoff; heat-up — лише Tunables + вибір профілю; панель без змін.

### Implementation delta (після plan refresh session 4)

Код `scripts/013_main_brew_logic.cpp` і docs ще відображають pre–session-4 поведінку. Після цього plan — оновити Tunables + FSM за [contracts/main-fsm.md](./contracts/main-fsm.md) / [tunable-constants.md](./contracts/tunable-constants.md), `docs/main-brew-logic.md`, потім `/speckit-tasks` (delta) або implement.

Ключові code deltas:
1. `STEAM_IDLE_EXIT_MS=0`; прибрати idle→`LT1` з `STEAM_READY`.
2. `B1`/`B2` у `HS1`/`HS0`/`STEAM_FLOW` → `coolOrReady()` / `LT1`; `B3` у цих станах — ignore.
3. `BREW_1` → `COLD_FLUSH`: `P11`, 4 с @ 100 %, ТЕН OFF; достроковий `B1` → `P10`; блок під час `BREW_2`/`steam`/`cool`.
4. `BREW_2` лишається єдиним еспресо з преінфузією + brew-flow у `FLOW`.
5. `STEAM_COOL`: ТЕН OFF; відкрита ручка → ті самі `STEAM_PULSE_*`; ready criterion → помпа 0 + `LT0` навіть при відкритій ручці.
6. Tunables: `HEATER_KP_HEAT`/`HEATER_KI_HEAT` (+ optional overshoot note); у `updateHeaterPi` вибирати heat-up у `ESPRESSO_HEAT`/`STEAM_HEAT`.
7. Синхронізувати `docs/main-brew-logic.md` і quickstart.

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
└── tasks.md             # Phase 12 delta (session 4) — CURRENT
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
└── main-brew-logic.md              # опис нової прошивки (синхронізувати з session 4)

specs/001-main-brew-logic/          # plan / research / model / contracts / quickstart
```

**Structure Decision**: Один новий скетч `scripts/013_main_brew_logic.cpp` у стилі існуючих стендів (монолітний `.cpp` з секціями констант / пінів / FSM / loop), плюс feature-docs у `specs/001-main-brew-logic/` і згодом `docs/main-brew-logic.md`. Окремі бібліотеки/пакети не виділяємо — обмеження flash/RAM і узгодженість з `006`/`012`.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
| --- | --- | --- |
| Constitution IV: монолітний `013_main_brew_logic.cpp` замість окремих C++ класів/модулів на драйвер | Узгодженість зі стендами `006`/`012`, ліміт Flash/RAM Nano, один артефакт прошивки основної плати | Окремі `.h`/класи на кожен драйвер збільшують обв’язку без виграшу для однієї плати; логічні секції + Tunables дають підтримуваність у межах фічі |

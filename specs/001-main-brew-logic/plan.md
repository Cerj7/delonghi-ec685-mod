# Implementation Plan: Main Brew Logic (EC685)

**Branch**: `001-main-brew-logic` | **Date**: 2026-11-10 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/001-main-brew-logic/spec.md`

**Note**: This template is filled in by the `/speckit-plan` command; its definition describes the execution workflow.

## Summary

Нова прошивка основної Arduino Nano, що відтворює штатну поведінку EC685: розігрів еспресо → готовність → пролив 1/2 з преінфузією → пар (120 °C, імпульсна помпа за ручкою) → охолодження → нагадування декальцинації раз на 28 діб. Технічний підхід: об’єднати ПІ-нагрів і диммер з `006_thermoblock_pi_heater.cpp`, UART-контракт панелі з `docs/button-panel-protocol.md` / `012_main_panel_link.cpp`, плюс FSM режимів, флоуметр, ручку пари та EEPROM-таймер `DC1`/`DC0`. Усі значущі числові параметри (температури, допуски, таймінги, потужності, ліміти захисту, інтервали) виносяться в іменовані константи на початку скетчу для швидкого тюнінгу.

## Technical Context

**Language/Version**: C++ (Arduino AVR toolchain / Arduino IDE), ATmega328P

**Primary Dependencies**: Adafruit GFX, Adafruit SSD1306, Adafruit MAX6675, AC_dimmer-dmr-0.2.1, SoftwareSerial, avr/wdt, EEPROM

**Storage**: EEPROM (накопичений uptime декальцинації: `accumulatedOnSeconds` + прапорець `dcLatched` / magic; інтервал 28×86400 с роботи)

**Testing**: Ручний стенд (дві Nano: панель `011` + основна нова прошивка); USB Serial 115200 для логів і сервісної команди `DC0`; сценарії з [quickstart.md](./quickstart.md)

**Target Platform**: Arduino Nano v3 (ATmega328P), кавоварка DeLonghi EC685 (мод)

**Project Type**: Embedded firmware (single sketch + docs)

**Performance Goals**: Відповідь на кнопку ≤300 мс; OLED/лічильник під час проливу ≥2 Гц; критерій готовності ±1 °C / ≥1.0 с; таймінги преінфузії/паузи/імпульсів пари ±200 мс

**Constraints**: Flash/RAM Nano; SoftwareSerial 19200 на D8/D9 (короткі кадри); захисний cutoff ТЕНу ≥ роботи на 120 °C; помпа 0 % поза проливом/паровою подачею; watchdog ~2 с

**Scale/Scope**: 1 нова прошивка основної плати + docs; панель `011` без змін контракту (лише споживач); ~1 FSM з 9 станами

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Конституція: `.specify/memory/constitution.md` **v1.0.0** (ratified 2026-11-11).

| Gate | Status | Notes |
| --- | --- | --- |
| I. Hardware Safety | PASS | Невалідна термопара / `SAFETY_CUTOFF_C` → ТЕН OFF, помпа 0, `FAULT` до power cycle |
| II. Memory Efficiency | PASS | Немає `malloc`/`new` у loop; EEPROM буфер фіксованого розміру; статичні буфери UART |
| III. Deterministic Real-Time | PASS | FSM + `millis()`; без блокуючих `delay()` у керуванні; ISR лише короткий лічильник імпульсів |
| IV. Maintainable & Modular | EXCEPTION | Монолітний `013` зі секціями (як `006`/`012`); див. Complexity Tracking |
| Контракт панелі / Tunables / YAGNI DC UI | PASS | Коди з protocol; усі °C/мс/% у Tunables; UI декальцинації out of scope |

**Post-design re-check**: PASS з задокументованим винятком IV — один скетч, логічні секції замість окремих класів/бібліотек.

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
└── tasks.md             # Phase 2 (/speckit-tasks) — створено
```

### Source Code (repository root)

```text
scripts/
├── 006_thermoblock_pi_heater.cpp   # база: ПІ, диммер, OLED, захист
├── 011_button_panel_ui.cpp         # панель (без змін у цій фічі)
├── 012_main_panel_link.cpp         # заглушка UART (референс лінку)
└── 013_main_brew_logic.cpp         # НОВА робоча прошивка основної плати

docs/
├── button-panel-protocol.md        # UART-контракт (вже є)
├── thermoblock-pi-heater.md        # референс ПІ/захисту
└── main-brew-logic.md              # опис нової прошивки (після імплементації)

specs/001-main-brew-logic/          # plan / research / model / contracts / quickstart
```

**Structure Decision**: Один новий скетч `scripts/013_main_brew_logic.cpp` у стилі існуючих стендів (монолітний `.cpp` з секціями констант / пінів / FSM / loop), плюс feature-docs у `specs/001-main-brew-logic/` і згодом `docs/main-brew-logic.md`. Окремі бібліотеки/пакети не виділяємо — обмеження flash/RAM і узгодженість з `006`/`012`.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
| --- | --- | --- |
| Constitution IV: монолітний `013_main_brew_logic.cpp` замість окремих C++ класів/модулів на драйвер | Узгодженість зі стендами `006`/`012`, ліміт Flash/RAM Nano, один артефакт прошивки основної плати | Окремі `.h`/класи на кожен драйвер збільшують обв’язку без виграшу для однієї плати; логічні секції + Tunables дають підтримуваність у межах фічі |

# Implementation Plan: Main Brew Logic (EC685)

**Branch**: `001-main-brew-logic` | **Date**: 2026-11-11 (plan refresh after clarify session 3) | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/001-main-brew-logic/spec.md`

**Note**: This template is filled in by the `/speckit-plan` command; its definition describes the execution workflow.

## Summary

Нова прошивка основної Arduino Nano, що відтворює штатну поведінку EC685: розігрів еспресо → готовність → пролив 1/2 з преінфузією → пар → охолодження → нагадування декальцинації раз на 28 діб. Технічний підхід: об’єднати ПІ-нагрів і диммер з `006_thermoblock_pi_heater.cpp`, UART-контракт панелі з `docs/button-panel-protocol.md` / `012_main_panel_link.cpp`, плюс FSM режимів, флоуметр, ручку пари та EEPROM-таймер `DC1`/`DC0`. Усі значущі числові параметри виносяться в Tunables на початку скетчу.

**Пар (clarify 2026-11-11 session 2)**:
- Готовність: `Tt ≥ STEAM_TARGET_C` (без ±2 / 118…122); overshoot OK.
- Після `HS0` утримувати ціль 120 °C; у `STEAM_READY` з закритою ручкою падіння → авто `HS1`→`HS0`; з уже відкритою ручкою після `HS0` — авто `SS1`.
- Закриття ручки: `SS0` → назад у `HS0` (не миттєвий `coolOrReady`).
- Idle `STEAM_IDLE_EXIT_MS=3000` (закрита ручка, без кнопок) → `LT1`, ціль пару знімається.
- `B3` під час `STEAM_COOL` → `enterSteam()` (`HS0`/`HS1`).

**Пар + нагрів (clarify 2026-11-11 session 3 — delta)**:
- У `STEAM_FLOW` (`SS1`, ручка відкрита): `Tt < STEAM_TARGET_C` **не** дає `HS1`/`SS0` і **не** паузить імпульсну помпу; лишаємось у flow, ТЕН гріє далі (FR-026).
- Три ПІ-профілі (FR-027 / SC-010, stand → option **C**): **base** · **brew-flow** 4× (`HEATER_KP_BREW`/`HEATER_KI_BREW` + `HEATER_FF_BREW_PCT`, лише `FLOW`) · **steam-flow** 2.5× (`HEATER_KP_STEAM`/`HEATER_KI_STEAM` + `HEATER_FF_STEAM_PCT`, увесь `STEAM_FLOW`). Без target-offset. Brew > steam > base. (3×/2× без FF — відхилено стендом.)

## Technical Context

**Language/Version**: C++ (Arduino AVR toolchain / Arduino IDE), ATmega328P

**Primary Dependencies**: Adafruit GFX, Adafruit SSD1306, Adafruit MAX6675, AC_dimmer-dmr-0.2.1, SoftwareSerial, avr/wdt, EEPROM

**Storage**: EEPROM (накопичений uptime декальцинації: `accumulatedOnSeconds` + прапорець `dcLatched` / magic; інтервал 28×86400 с роботи)

**Testing**: Ручний стенд (дві Nano: панель `011` + основна нова прошивка); USB Serial 115200 для логів і сервісної команди `DC0`; сценарії з [quickstart.md](./quickstart.md)

**Target Platform**: Arduino Nano v3 (ATmega328P), кавоварка DeLonghi EC685 (мод)

**Project Type**: Embedded firmware (single sketch + docs)

**Performance Goals**: Відповідь на кнопку ≤300 мс; OLED/лічильник під час проливу ≥2 Гц; еспресо-готовність ±1 °C / ≥1.0 с; пар-готовність `Tt ≥ STEAM_TARGET_C` миттєво; idle пару 10000 мс → `LT1`; таймінги преінфузії/паузи/імпульсів ±200 мс; перемикання ПІ-профілю ≤ одного `CONTROL_INTERVAL_MS`

**Constraints**: Flash/RAM Nano; SoftwareSerial 19200 на D8/D9 (короткі кадри); захисний cutoff ТЕНу ≥ роботи на 120 °C; помпа 0 % поза проливом/паровою подачею; watchdog ~2 с; агресивніші Kp/Ki під час flow не знімають `SAFETY_CUTOFF_C`

**Scale/Scope**: 1 нова прошивка основної плати + docs; панель `011` без змін контракту (лише споживач); ~1 FSM з 9 станами; 3 ПІ-профілі нагріву

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Конституція: `.specify/memory/constitution.md` **v1.0.0** (ratified 2026-11-11).

| Gate | Status | Notes |
| --- | --- | --- |
| I. Hardware Safety | PASS | Невалідна термопара / `SAFETY_CUTOFF_C` → ТЕН OFF, помпа 0, `FAULT` до power cycle; агресивні профілі не обходять cutoff |
| II. Memory Efficiency | PASS | Немає `malloc`/`new` у loop; EEPROM буфер фіксованого розміру; статичні буфери UART |
| III. Deterministic Real-Time | PASS | FSM + `millis()`; без блокуючих `delay()` у керуванні; ISR лише короткий лічильник імпульсів |
| IV. Maintainable & Modular | EXCEPTION | Монолітний `013` зі секціями (як `006`/`012`); див. Complexity Tracking |
| Контракт панелі / Tunables / YAGNI DC UI | PASS | Коди з protocol; усі °C/мс/%/Kp/Ki у Tunables; UI декальцинації out of scope |

**Post-design re-check** (2026-11-11 session 3): PASS з задокументованим винятком IV. Session 3 delta не змінює gates: continuous steam pulse лишає cutoff; три профілі — лише Tunables + вибір Kp/Ki у PI step.

### Implementation delta (після plan refresh)

Код `scripts/013_main_brew_logic.cpp` ще має: (а) паузу помпи/`HS1` при `Tt < target` у `STEAM_FLOW`; (б) один набір `HEATER_KP`/`HEATER_KI`. Після цього plan — оновити Tunables + FSM за [contracts/main-fsm.md](./contracts/main-fsm.md) / [tunable-constants.md](./contracts/tunable-constants.md), `docs/main-brew-logic.md`, потім `/speckit-tasks` (delta) або прямий implement.

Ключові code deltas:
1. `STEAM_FLOW`: прибрати гілку `Tt < STEAM_TARGET_C` → `SS0`/`STEAM_HEAT`; гріти steam-flow профілем, імпульси не чіпати.
2. Tunables: brew 4× + FF, steam 2.5× + FF; у `updateHeaterPi` вибирати профіль і додавати FF лише у brew-`FLOW` / `STEAM_FLOW`.
3. Авто-`HS1` лише з `STEAM_READY` (ручка закрита).

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
└── tasks.md             # Phase 2 (/speckit-tasks) — потрібен delta session 3
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
└── main-brew-logic.md              # опис нової прошивки (синхронізувати з FR-026/027)

specs/001-main-brew-logic/          # plan / research / model / contracts / quickstart
```

**Structure Decision**: Один новий скетч `scripts/013_main_brew_logic.cpp` у стилі існуючих стендів (монолітний `.cpp` з секціями констант / пінів / FSM / loop), плюс feature-docs у `specs/001-main-brew-logic/` і згодом `docs/main-brew-logic.md`. Окремі бібліотеки/пакети не виділяємо — обмеження flash/RAM і узгодженість з `006`/`012`.

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
| --- | --- | --- |
| Constitution IV: монолітний `013_main_brew_logic.cpp` замість окремих C++ класів/модулів на драйвер | Узгодженість зі стендами `006`/`012`, ліміт Flash/RAM Nano, один артефакт прошивки основної плати | Окремі `.h`/класи на кожен драйвер збільшують обв’язку без виграшу для однієї плати; логічні секції + Tunables дають підтримуваність у межах фічі |

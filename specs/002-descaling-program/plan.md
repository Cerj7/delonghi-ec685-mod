# Implementation Plan: Descaling Program (EC685)

**Branch**: `002-descaling-program` (git: `decalcination-mode`) | **Date**: 2026-11-26 | **Spec**: [spec.md](./spec.md)

**Input**: Feature specification from `/specs/002-descaling-program/spec.md`

**Note**: This template is filled in by the `/speckit-plan` command; its definition describes the execution workflow.

## Summary

Повна програма декальцинації поверх `001-main-brew-logic`: вхід утриманням «пар» 10 с (з `DC1` і без), OLED-підказки українською, дві імпульсні фази (очистка → промивання) з об’ємом і no-flow, **політика ТЕНа**: у активній фазі 1 уставка `DESCALE_PHASE1_TEMP_C` = **53 °C** (помпа + ПІ паралельно, без temperature-gate); у підготовці / доливах / wait-rinse / фазі 2 — ТЕН **0 %**; скидання нагадування лише після успішної фази 2 через існуючий `DC0`-ефект. Технічний підхід: розширити `011`/`013` контрактом press+release для `B3`, додати FSM-стани декальцинації в `013`, Tunables `DESCALE_*` (включно з температурою фази 1), reuse `handleServiceDc0()` після фази 2.

## Technical Context

**Language/Version**: C++ (Arduino AVR toolchain / Arduino IDE), ATmega328P

**Primary Dependencies**: Adafruit GFX, Adafruit SSD1306, Adafruit MAX6675, AC_dimmer-dmr-0.2.1, SoftwareSerial, avr/wdt, EEPROM (як у `013`)

**Storage**: EEPROM блоку декальцинації з `001` (`accumulatedOnSeconds`, `dcLatched`); програма v1 **не** персистить фазу після power cycle

**Testing**: Ручний стенд (панель `011` + основна `013`); USB Serial 115200 (логи + сервісний `DC0`); сценарії [quickstart.md](./quickstart.md); тимчасово зменшені `DESCALE_*` / `DECALC_INTERVAL_S` для стенду

**Target Platform**: Arduino Nano v3 (ATmega328P), DeLonghi EC685 (мод)

**Project Type**: Embedded firmware (розширення існуючих скетчів + feature docs)

**Performance Goals**: Відлік OLED після ≥1 с утримання; вхід ±200 мс від 10 с; імпульси фаз ±50 мс; `DC0` після фази 2 ≤1 с; реакція на «пар»/ручку в межах існуючого SLA ≤300 мс

**Constraints**: Flash/RAM Nano; `LINK_CODE_MAX_LEN=3`; SoftwareSerial 19200; без `delay()` у керуванні; ТЕН **53 °C лише в `DESCALE_PHASE1`**, інакше 0 % у програмі (не еспресо-/парова уставка); `B1`/`B2` ігнор під час програми; no-flow лише під час ON помпи; об’єм потребує `FLOW_PULSES_PER_ML > 0`

**Scale/Scope**: Оновлення `011` (release `R3`) + `013` (FSM/UI/імпульси/об’єм/уставка фази 1); docs protocol + `main-brew-logic`; ~6–7 нових станів FSM; без окремих бібліотек

## Constitution Check

*GATE: Must pass before Phase 0 research. Re-check after Phase 1 design.*

Конституція: `.specify/memory/constitution.md` **v1.0.0** (ratified 2026-11-11).

| Gate | Status | Notes |
| --- | --- | --- |
| I. Hardware Safety | PASS | Фаза 1: контрольована уставка **53 °C** (як штатний PIC); prep/refill/wait/фаза 2: ТЕН 0 %; `FAULT` → ТЕН/помпа OFF, без `DC0`; без еспресо-/парових уставок під кислотою |
| II. Memory Efficiency | PASS | Статичні буфери OLED/UART; без `malloc`/`new` у loop; EEPROM лише існуючий блок `001` |
| III. Deterministic Real-Time | PASS | FSM + `millis()`; hold/countdown/імпульси/no-flow/ПІ без блокуючих `delay()`; ISR флоуметра лишається коротким; **немає** блокуючого wait-for-temp перед помпою |
| IV. Maintainable & Modular | EXCEPTION | Продовження моноліту `013` (+ мінімальна зміна `011`); див. Complexity Tracking (успадковано з `001`) |
| Tunables / YAGNI / panel dumb | PASS | Усі `DESCALE_*` (вкл. `DESCALE_PHASE1_TEMP_C`) у Tunables; панель не вирішує режим (лише `B3`/`R3`); cancel-жест v1 не додаємо |

**Post-design re-check** (2026-11-26, clarify sync): PASS з тим самим винятком IV. Контракти Phase 1: динамічної пам’яті немає; temperature-gate перед помпою відсутній; нагрів обмежений станом `DESCALE_PHASE1` і Tunable 53 °C.

## Project Structure

### Documentation (this feature)

```text
specs/002-descaling-program/
├── plan.md              # This file
├── research.md          # Phase 0
├── data-model.md        # Phase 1
├── quickstart.md        # Phase 1
├── contracts/
│   ├── descale-fsm.md
│   ├── panel-hold-uart.md
│   └── descale-tunables.md
├── checklists/
│   └── requirements.md
└── tasks.md             # Phase 2 (/speckit-tasks) — NOT created here
```

### Source Code (repository root)

```text
scripts/
├── 011_button_panel_ui.cpp     # DELTA: UART release `R3` для «пар»
├── 013_main_brew_logic.cpp     # DELTA: hold-enter, descale FSM, OLED, heater gate, DC0 після фази 2

docs/
├── button-panel-protocol.md    # DELTA: press/release для B3 / R3
└── main-brew-logic.md          # DELTA: програма декальцинації

specs/001-main-brew-logic/      # База: DecalcTimer, USB DC0, steam/brew FSM, PI heater (reuse)
docs/pic16f1938-operating-modes.md  # Довідка: штатна уставка декальцинації ≈53 °C (ADC 0x0177)
```

**Structure Decision**: Розширюємо існуючі скетчі `011`/`013` і shared docs протоколу; feature-артефакти живуть у `specs/002-descaling-program/`. Окремий модуль/клас не виділяємо (той самий pragmatic exception, що в `001`).

## Complexity Tracking

| Violation | Why Needed | Simpler Alternative Rejected Because |
| --- | --- | --- |
| Constitution IV: монолітний `013` (+ точковий `011`) замість окремих C++ модулів | Узгодженість з `001`/`006`/`012`, ліміт Flash/RAM, один артефакт основної плати | Винесення descale в окремий `.h`/клас без інфраструктури тестів не зменшує зв’язність з FSM/heater/pump і роздуває обв’язку на Nano |

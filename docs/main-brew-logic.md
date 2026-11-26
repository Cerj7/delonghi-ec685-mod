# Main Brew Logic (`scripts/013_main_brew_logic.cpp`)

Прошивка основної Arduino Nano: відтворення штатної поведінки DeLonghi EC685 (розігрів → холодний пролив `B1` → еспресо `B2` з преінфузією → пар → охолодження → нагадування декальцинації). База — ПІ/диммер/OLED з [`006_thermoblock_pi_heater.cpp`](../scripts/006_thermoblock_pi_heater.cpp); події кнопок і LED — через UART-панель за [`button-panel-protocol.md`](button-panel-protocol.md). Панель `011` **без змін**.

Специфікація фічі: [`specs/001-main-brew-logic/`](../specs/001-main-brew-logic/) (`spec.md`, `plan.md`, `contracts/`, `quickstart.md`).

**Clarify 2026-11-15 (Phase 13):** dual steam targets (`STEAM_READY_TARGET_C=120` / `STEAM_FLOW_TARGET_C=128`), діапазони pot (A1 85…105 °C, A0 50…100 %), OLED **field sets** (brew metrics / cold-flush countdown / steam+cool). Session 4 без змін: cold flush, вихід з пару лише `B1`/`B2`, idle/`B3`-cancel вимкнено, `STEAM_COOL`+ручка → імпульси / ТЕН OFF, чотири ПІ-профілі.

**Clarify 2026-11-15 (Phase 14):** OLED `ESPRESSO_HEAT`/`ESPRESSO_READY` (`HE1`/`HE0`) — **FR-034** (5 полів / 4 рядки, у т.ч. `Pump_target`); спільний словник labels для brew/steam (короткі aliases на 128×32). Phase 13 field sets **лишаються**, лише з новими іменами labels. **Стендові ПІ** (`HEATER_KP_*` / `HEATER_KI_*` / `HEATER_FF_*`) у Phase 13/14 **без змін**. Панель `011` **без змін**.

| Пункт | Реалізація |
| --- | --- |
| Призначення | FSM режимів на основній платі + лінк до панелі `011` |
| База апаратки | як `006` (ТЕН `D4`, диммер, MAX6675, OLED, потенціометри) |
| Панель | `SoftwareSerial` на `D8`/`D9` @ `19200` (як `012`); прошивка `011` незмінна |
| Параметри | секція **Tunables** на початку скетчу |
| Watchdog | 2 с (як у `006`) |
| USB | `115200` — CSV-лог + сервісні `DC0`/`DC1` |
| FAULT | ТЕН OFF, помпа 0; вихід лише power cycle |

---

## Піни: відмінності від `006`

Усе спільне з [`thermoblock-pi-heater.md`](thermoblock-pi-heater.md) лишається. Зміни лише там, де `006` тримав заглушки під кнопки:

| Пін | У `006` | У `013` |
| --- | --- | --- |
| `D8` | кнопка (не використовується, `INPUT_PULLUP`) | `PANEL_TX` → `D5` панелі |
| `D9` | кнопка (не використовується) | `PANEL_RX` ← `D6` панелі |
| `D5` | флоуметр — заглушка | флоуметр (лічильник імпульсів; див. Tunables) |
| `D10` | ручка пари — заглушка | ручка пари (вхід для `STEAM_FLOW` / cool-імпульсів) |
| решта (`D2`/`D3`/`D4`, `A0`/`A1`, термопари, OLED) | без змін щодо `006` | без змін |

> Спільний `GND` лише між двома Nano; шлейф панелі від’єднати від штатного `J8` — як у [`button-panel-ui.md`](button-panel-ui.md).

---

## FSM (коротко)

Стани `MachineState` (без `COLD_START`; `BREW_1` замінено на `COLD_FLUSH`). Повна матриця кнопок і `coolOrReady()` — [`contracts/main-fsm.md`](../specs/001-main-brew-logic/contracts/main-fsm.md).

```
POWER_ON → HE1 → ESPRESSO_HEAT  // PI: heat-up; target = pot A1 (85…105 °C)
                 │ OLED FR-034 @ BREW_DISPLAY_INTERVAL_MS (T_now/T_tar/Heat/P_now+P_tar)
                 │ ready(Tt ≥ target−READY_BAND_C, READY_HOLD_MS; no upper)
                 ▼
            ESPRESSO_READY (HE0)  // PI: base; OLED same FR-034 (incl. after P10/P20/LT0)
                 ├─ B1 → COLD_FLUSH (P11): COLD_FLUSH_MS @ COLD_FLUSH_PCT; ТЕН OFF
                 │     OLED: "Cold flush N s" (countdown; FR-033)
                 │     stop: timeout або B1 → P10 → ESPRESSO_READY; ignore B2/B3
                 ├─ B2 → BREW_2 (P21): PREINFUSE → PAUSE → FLOW; stop B2 → P20
                 │     pump FLOW: pot A0 → PUMP_MIN_PCT…PUMP_MAX_PCT (50…100)
                 │     OLED: T_now / T_tar / Heat / P_now (FR-008; no P_tar)
                 │     PI: PREINFUSE/PAUSE = base; FLOW = brew-flow (~5× + FF)
                 └─ B3 → enterSteam()  // Tt ≥ STEAM_READY_TARGET_C (120) → HS0, інакше HS1
                            ├─ STEAM_HEAT: target 120; Tt ≥ 120 → HS0 (без hold / без ± band)
                            │     if handle open → SS1 @ 128; B1/B2 → coolOrReady(); B3 → ignore
                            │     PI: heat-up; OLED: T_now / Heat / V open|V close
                            ├─ STEAM_READY (HS0): maintain STEAM_READY_TARGET_C (120); PI = base
                            │     Tt < 120 (ручка closed) → HS1 (авто); handle open → SS1 @ 128
                            │     idle вимкнено (STEAM_IDLE_EXIT_MS=0) — НЕМАЄ auto-LT1
                            │     B1/B2 → coolOrReady(); B3 → ignore  // НЕ SS1, НЕ cancel
                            │     OLED: T_now / Heat / V open|V close
                            ├─ STEAM_FLOW: activeTargetC = STEAM_FLOW_TARGET_C (128)
                            │     close → SS0, target→120 → STEAM_READY (не coolOrReady)
                            │     Tt < 128 (або навіть < 120) → лишатись у flow (без HS1 / без паузи)
                            │     B1/B2 → coolOrReady(); B3 → ignore
                            │     PI = steam-flow (~3× + FF); OLED: T_now / Heat / V open|V close
                            └─ STEAM_COOL (LT1): ТЕН OFF; handle open → STEAM_PULSE_*
                                  cool-ready(Tt ≤ espressoTarget+READY_BAND_C, hold) → pump 0 → LT0
                                  B3 → enterSteam(); B1/B2 → ignore
                                  OLED: T_now / Heat(=0) / V open|V close (FR-029)

FAULT ← невалідна Tt / SAFETY_CUTOFF_C  (лише power cycle)
```

Ключові правила:

- `B1` з `ESPRESSO_READY` = **холодний пролив** (`COLD_FLUSH`), не еспресо; єдине еспресо — `B2` / `BREW_2`. Flush не стартує з `BREW_2` / `STEAM_HEAT` / `STEAM_FLOW` / `STEAM_COOL`; у `HS0`/`SS*` `B1` = steam-exit, не flush.
- Під час `COLD_FLUSH` чужі кнопки (`B2`/`B3`) ігнор; під час `BREW_2` — `B1`/`B3`.
- Вихід з пару **лише** `B1`/`B2` у `STEAM_HEAT` / `STEAM_READY` / `STEAM_FLOW` → `coolOrReady()` → `LT1`. `B3` у цих станах **ігнор** (не cancel, не `SS1`). У `STEAM_COOL` `B3` = `enterSteam()`; `B1`/`B2` ігнор.
- Idle-автовихід вимкнено: `STEAM_IDLE_EXIT_MS=0` — закрита ручка без кнопок **не** дає `LT1`.
- **Dual steam targets (FR-032):** `STEAM_READY_TARGET_C=120` для `STEAM_HEAT` / `STEAM_READY` / `HS0` / auto-`HS1`; `STEAM_FLOW_TARGET_C=128` увесь час відкритої ручки / `STEAM_FLOW`. Legacy `STEAM_TARGET_C` = alias ready (120); **не** ціль у flow. `SAFETY_CUTOFF_C` MUST бути строго вище 128.
- Потенціометри: A1 → `TARGET_MIN_C`…`TARGET_MAX_C` (**85…105 °C**); A0 → `PUMP_MIN_PCT`…`PUMP_MAX_PCT` (**50…100 %**) live у `BREW_2` FLOW.
- Помпа: flush `COLD_FLUSH_PCT`; преінфузія `PREINFUSION_PCT`; FLOW — живий потенціометр `A0` (50…100); пар / cool з відкритою ручкою — імпульси `STEAM_PULSE_*` (навіть якщо Tt < 128 у flow); на espresso-ready у cool — помпа 0 навіть з відкритою ручкою; інакше `PUMP_OFF_PCT`.
- ПІ-профілі (FR-027 / SC-010/012) — **чотири** tunable набори Kp/Ki + FF під час flow, без target-offset. **Phase 13/14 стендові значення не змінювали** — лишилися як у `013` / після T041:
  - **heat-up** (`HEATER_KP_HEAT`/`HEATER_KI_HEAT` = 12.0/0.12, ~2.4× base, FF=0) — `ESPRESSO_HEAT` / `STEAM_HEAT`; короткочасний overshoot ~+3…5 °C OK (`HEAT_UP_OVERSHOOT_OK_C`);
  - **base** (`HEATER_KP`/`HEATER_KI` = 5.0/0.05, FF=0) — idle / ready / brew `PREINFUSE`/`PAUSE` / `STEAM_READY`;
  - **brew-flow** (`HEATER_KP_BREW`/`HEATER_KI_BREW` = 25.0/0.25, **~5×** stand + `HEATER_FF_BREW_PCT=25`) — лише `BREW_2` phase `FLOW`;
  - **steam-flow** (`HEATER_KP_STEAM`/`HEATER_KI_STEAM` = 15.0/0.15, **~3×** stand + `HEATER_FF_STEAM_PCT=15`) — увесь `STEAM_FLOW`;
  - **forced OFF** — `COLD_FLUSH` / `STEAM_COOL` / `FAULT`.
  - *(Option C стартувало 4×/2.5×+FF; T041 stand-tune → дефолти як вище / `contracts/tunable-constants.md`. Phase 13 = dual targets / pot / OLED fields; Phase 14 = FR-034 + shared labels.)*
- `coolOrReady()`: лише steam-exit через `B1`/`B2` → зупинка імпульсів → `LT1`/`STEAM_COOL` з ТЕН OFF. **Не** з `SS0`, **не** з idle, **не** з `B3`, **не** з падіння Tt у `STEAM_FLOW`.
- Готовність пару: `Tt ≥ STEAM_READY_TARGET_C` (120; overshoot OK; без `READY_HOLD_MS`, без ±`STEAM_READY_BAND_C`). У `STEAM_FLOW` ціль 128; падіння Tt **не** дає `HS1`/`SS0` і **не** паузить помпу (FR-026); авто-`HS1` лише з `STEAM_READY` при закритій ручці.

---

## OLED layouts (FR-034 / FR-008 / FR-029 / FR-033)

Оновлення heat/ready/brew/flush — `BREW_DISPLAY_INTERVAL_MS`; пар/cool — `OLED_INTERVAL_MS`. Деталі — [`contracts/main-fsm.md`](../specs/001-main-brew-logic/contracts/main-fsm.md) § OLED authority; словник — [`contracts/tunable-constants.md`](../specs/001-main-brew-logic/contracts/tunable-constants.md) § OLED labels.

**Phase 13 field sets лишаються** (які значення показувати); Phase 14 лише замінює імена labels і додає FR-034 для `HE1`/`HE0`. У `013` на 128×32 використовуються **short** aliases.

| Стан | Поля (full \| short у коді) | Примітки |
| --- | --- | --- |
| `ESPRESSO_HEAT` / `ESPRESSO_READY` | 5 полів / **4 рядки**: `T_current`\|`T_now` → `T_target`\|`T_tar` → `Heat_power`\|`Heat` → (`Pump_current`\|`P_now` + `Pump_target`\|`P_tar`) | FR-034 / SC-017; `showEspressoHeatReadyOled()`; `P_tar` з pot A0; **не** legacy `006` idle |
| `BREW_2` | 4 поля: `T_current`\|`T_now` / `T_target`\|`T_tar` / `Heat_power`\|`Heat` / `Pump_current`\|`P_now` | FR-008 / SC-004; **без** `Pump_target`; лічильник води на OLED **не** обов’язковий |
| `COLD_FLUSH` | `Cold flush N s` | FR-033 / SC-016; `N = ceil(remainingMs/1000)`; **не** brew/heat layout |
| `STEAM_HEAT` / `STEAM_READY` / `STEAM_FLOW` / `STEAM_COOL` | `T_current`\|`T_now` / `Heat_power`\|`Heat` / `Valve open`\|`Valve close` (short `V open`\|`V close`) | FR-029 / SC-013; у `STEAM_COOL` `Heat` MUST = 0 |
| `FAULT` | fault | ТЕН OFF, помпа 0 |

**Спільний словник** (заборонені сирі `Tt`/`S`/`H`/`Pp`/`Ps` і голі `open`/`close` на espresso/steam екранах):

| Full | Short | Де |
| --- | --- | --- |
| `T_current` | `T_now` | FR-008, FR-029, FR-034 |
| `T_target` | `T_tar` | FR-008, FR-034 |
| `Heat_power` | `Heat` | FR-008, FR-029, FR-034 |
| `Pump_current` | `P_now` | FR-008, FR-034 |
| `Pump_target` | `P_tar` | **лише FR-034** |
| `Valve open` / `Valve close` | `V open` / `V close` | FR-029 |

---

## Tunables

Усі температури, таймінги, потужності, PI, декальцинація, лінк і `FLOW_PULSES_PER_ML` — **лише** в секції Tunables на початку [`scripts/013_main_brew_logic.cpp`](../scripts/013_main_brew_logic.cpp).

Каталог констант і дефолти (таблиці Temperatures / Timing / Powers / PI / Decalc / Link):  
[`specs/001-main-brew-logic/contracts/tunable-constants.md`](../specs/001-main-brew-logic/contracts/tunable-constants.md).

Орієнтири 2026-11-15:

| Група | Константи |
| --- | --- |
| Espresso pot A1 | `TARGET_MIN_C=85`, `TARGET_MAX_C=105` |
| Pump pot A0 | `PUMP_MIN_PCT=50`, `PUMP_MAX_PCT=100` |
| Steam dual | `STEAM_READY_TARGET_C=120`, `STEAM_FLOW_TARGET_C=128` (`STEAM_TARGET_C` = alias ready) |
| Safety | `SAFETY_CUTOFF_C=145` (> 128) |
| Cold flush | `COLD_FLUSH_MS` (стенд), `COLD_FLUSH_PCT=100`, `STEAM_IDLE_EXIT_MS=0` |
| PI (unchanged у Phase 13/14) | `HEATER_KP_*` / `HEATER_KI_*` / `HEATER_FF_*` — стенд у `013` |

Тюнінг стенду = правка Tunables (+ за потреби синхрон контракту), без «магічних» літералів у тілі FSM. **Не відкочувати** ПІ під старі табличні 4×/2.5× у цій фазі; Phase 14 не чіпає Kp/Ki/FF.

---

## Serial service (USB → main)

USB Serial основної: **115200**, кадр = код ≤3 символи + `\n` (як panel UART). Контракт: [`contracts/panel-uart.md`](../specs/001-main-brew-logic/contracts/panel-uart.md) § Service Serial.

| Команда | Дія |
| --- | --- |
| `DC0` | Надіслати панелі `DC0`, зняти `dcLatched`, обнулити `accumulatedOnSeconds`, зберегти EEPROM |
| `DC1` | (стенд) Примусово надіслати `DC1`, встановити latch і persist |
| інше | Ігнор + лог `# service (unknown) …` |

Періодичний CSV-лог (стан, Tt, уставка, ТЕН %, помпа) — окремо від сервісних команд; не частина міжплатного контракту.

Автоматичний `DC1` з FSM: коли накопичений **uptime** ≥ `DECALC_INTERVAL_S` і ще не latched — один кадр на панель (UI декальцинації на панелі out of scope).

---

## Стенд: wiring

Дві Arduino Nano: панель [`011_button_panel_ui.cpp`](../scripts/011_button_panel_ui.cpp) + основна `013`. Прошивка панелі **не** змінюється (session 4 / 2026-11-15).

| Зв’язок | Основна (`013`) | Панель (`011`) |
| --- | --- | --- |
| UART TX | `D8` | → `D5` (RX) |
| UART RX | `D9` | ← `D6` (TX) |
| GND | спільний | спільний |

Решта основної плати — як у `006` / [`thermoblock-pi-heater.md`](thermoblock-pi-heater.md):

- ТЕН SSR `D4`, диммер ZC `D2` / GATE `D3`, потенціометри `A0` (помпа **50…100 %**) / `A1` (уставка еспресо **85…105 °C**)
- MAX6675 термоблок `CS=A2`, бачок `CS=D11` обов’язково `HIGH`
- OLED I2C `A4`/`A5`
- Флоуметр `D5`, ручка пари `D10`

Порядок заливки й сценарії S1–S7 (+ SC-013…SC-017): [`quickstart.md`](../specs/001-main-brew-logic/quickstart.md).

> ⚠️ Шлейф панелі **не** підключати до штатного `J8`. GND лише між двома Nano, не зі штатною платою кавоварки.

---

## Відмінність від прикладу протоколу: `B3` @ `HS*` / `SS*`

У [`button-panel-protocol.md`](button-panel-protocol.md) ілюстративний сценарій показує: у стані «пар готовий» (`HS0`, LED3 постійно) повторне натискання «Пар» (`B3`) → основна шле `SS1` (старт подачі).

**У `013` / цій фічі це не так:**

| Джерело | `B3` у `HS0` / `HS1` / `SS*` | Старт `SS1` | Вихід з пару (`LT1`) |
| --- | --- | --- | --- |
| Приклад у protocol | → `SS1` | від кнопки «Пар» | (залежить від прикладу) |
| `013` (FR-021/022) | → **ignore** (не cancel, не `SS1`) | відкриття **ручки пари** (або одразу після `HS0`, якщо ручка вже відкрита) → ціль **128** | лише `B1` або `B2` → `coolOrReady()` |

Тобто коди кадрів ті самі (`B1`/`B2`/`B3`, `HS*`, `SS*`, `LT*`, `P11`/`P10` для flush), але семантика основної: `B3`@пар = ігнор; `SS1` лише від ручки (з ціллю 128); cancel = `B1`/`B2`. Фіксація override: [`contracts/panel-uart.md`](../specs/001-main-brew-logic/contracts/panel-uart.md) § Feature-specific overrides.

---

## Посилання

- Фіча: [`specs/001-main-brew-logic/`](../specs/001-main-brew-logic/)
- FSM: [`contracts/main-fsm.md`](../specs/001-main-brew-logic/contracts/main-fsm.md)
- Tunables: [`contracts/tunable-constants.md`](../specs/001-main-brew-logic/contracts/tunable-constants.md)
- Panel UART (споживач): [`contracts/panel-uart.md`](../specs/001-main-brew-logic/contracts/panel-uart.md)
- Контракт панелі: [`button-panel-protocol.md`](button-panel-protocol.md)
- База нагріву: [`thermoblock-pi-heater.md`](thermoblock-pi-heater.md) / `006`
- Тестовий лінк: [`scripts/012_main_panel_link.cpp`](../scripts/012_main_panel_link.cpp)
- Стенд: [`quickstart.md`](../specs/001-main-brew-logic/quickstart.md)

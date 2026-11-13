# Main Brew Logic (`scripts/013_main_brew_logic.cpp`)

Прошивка основної Arduino Nano: відтворення штатної поведінки DeLonghi EC685 (розігрів → холодний пролив `B1` → еспресо `B2` з преінфузією → пар → охолодження → нагадування декальцинації). База — ПІ/диммер/OLED з [`006_thermoblock_pi_heater.cpp`](../scripts/006_thermoblock_pi_heater.cpp); події кнопок і LED — через UART-панель за [`button-panel-protocol.md`](button-panel-protocol.md). Панель `011` **без змін** (session 4 лише семантика основної).

Специфікація фічі: [`specs/001-main-brew-logic/`](../specs/001-main-brew-logic/) (`spec.md`, `plan.md`, `contracts/`, `quickstart.md`). Clarify session 4: cold flush, вихід з пару лише `B1`/`B2`, idle/`B3`-cancel вимкнено, `STEAM_COOL`+ручка → імпульси / ТЕН OFF, чотири ПІ-профілі.

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
POWER_ON → HE1 → ESPRESSO_HEAT  // PI: heat-up
                 │ ready(Tt ≥ target−READY_BAND_C, READY_HOLD_MS; no upper)
                 ▼
            ESPRESSO_READY (HE0)  // PI: base
                 ├─ B1 → COLD_FLUSH (P11): COLD_FLUSH_MS @ COLD_FLUSH_PCT; ТЕН OFF
                 │     stop: timeout або B1 → P10 → ESPRESSO_READY; ignore B2/B3
                 ├─ B2 → BREW_2 (P21): PREINFUSE → PAUSE → FLOW; stop B2 → P20
                            │     PI: PREINFUSE/PAUSE = base; FLOW = brew-flow (~5× + FF)
                 └─ B3 → enterSteam()  // Tt ≥ STEAM_TARGET_C → HS0, інакше HS1
                            ├─ STEAM_HEAT: Tt ≥ STEAM_TARGET_C → HS0 (без hold / без ± band)
                            │     if handle open → SS1; B1/B2 → coolOrReady(); B3 → ignore
                            │     PI: heat-up
                            ├─ STEAM_READY (HS0): ціль STEAM_TARGET_C; PI = base
                            │     Tt < target (ручка closed) → HS1 (авто); handle open → SS1
                            │     idle вимкнено (STEAM_IDLE_EXIT_MS=0) — НЕМАЄ auto-LT1
                            │     B1/B2 → coolOrReady(); B3 → ignore  // НЕ SS1, НЕ cancel
                            ├─ STEAM_FLOW: close → SS0 → HS0 (не coolOrReady)
                            │     Tt < target → лишатись у flow (без HS1 / без паузи помпи)
                            │     B1/B2 → coolOrReady(); B3 → ignore
                            │     PI = steam-flow (~4× + FF)
                            └─ STEAM_COOL (LT1): ТЕН OFF; handle open → STEAM_PULSE_*
                                  cool-ready(Tt ≤ target+READY_BAND_C, hold) → pump 0 → LT0
                                  B3 → enterSteam(); B1/B2 → ignore (не flush / не brew)

FAULT ← невалідна Tt / SAFETY_CUTOFF_C  (лише power cycle)
```

Ключові правила:

- `B1` з `ESPRESSO_READY` = **холодний пролив** (`COLD_FLUSH`), не еспресо; єдине еспресо — `B2` / `BREW_2`. Flush не стартує з `BREW_2` / `STEAM_HEAT` / `STEAM_FLOW` / `STEAM_COOL`; у `HS0`/`SS*` `B1` = steam-exit, не flush.
- Під час `COLD_FLUSH` чужі кнопки (`B2`/`B3`) ігнор; під час `BREW_2` — `B1`/`B3`.
- Вихід з пару **лише** `B1`/`B2` у `STEAM_HEAT` / `STEAM_READY` / `STEAM_FLOW` → `coolOrReady()` → `LT1`. `B3` у цих станах **ігнор** (не cancel, не `SS1`). У `STEAM_COOL` `B3` = `enterSteam()`; `B1`/`B2` ігнор.
- Idle-автовихід вимкнено: `STEAM_IDLE_EXIT_MS=0` — закрита ручка без кнопок **не** дає `LT1`.
- Помпа: flush `COLD_FLUSH_PCT`; преінфузія `PREINFUSION_PCT`; FLOW — живий потенціометр `A0`; пар / cool з відкритою ручкою — імпульси `STEAM_PULSE_*` (навіть якщо Tt < target у flow); на espresso-ready у cool — помпа 0 навіть з відкритою ручкою; інакше `PUMP_OFF_PCT`.
- ПІ-профілі (FR-027 / SC-010/012) — **чотири** tunable набори Kp/Ki + FF під час flow, без target-offset:
  - **heat-up** (`HEATER_KP_HEAT`/`HEATER_KI_HEAT`, ~2.4× base, FF=0) — `ESPRESSO_HEAT` / `STEAM_HEAT`; короткочасний overshoot ~+3…5 °C OK (`HEAT_UP_OVERSHOOT_OK_C`);
  - **base** (`HEATER_KP`/`HEATER_KI` = 5.0/0.05, FF=0) — idle / ready / brew `PREINFUSE`/`PAUSE` / `STEAM_READY`;
  - **brew-flow** (`HEATER_KP_BREW`/`HEATER_KI_BREW` = 25.0/0.25, **~5×** stand + `HEATER_FF_BREW_PCT=25`) — лише `BREW_2` phase `FLOW`;
  - **steam-flow** (`HEATER_KP_STEAM`/`HEATER_KI_STEAM` = 20.0/0.20, **~4×** stand + `HEATER_FF_STEAM_PCT=20`) — увесь `STEAM_FLOW`;
  - **forced OFF** — `COLD_FLUSH` / `STEAM_COOL` / `FAULT`.
  - *(Option C стартувало 4×/2.5×+FF20/12; після T041 stand-tune — дефолти як вище / `contracts/tunable-constants.md`.)*
- `coolOrReady()`: лише steam-exit через `B1`/`B2` → зупинка імпульсів → `LT1`/`STEAM_COOL` з ТЕН OFF. **Не** з `SS0`, **не** з idle, **не** з `B3`, **не** з падіння Tt у `STEAM_FLOW`.
- Готовність пару: `Tt ≥ STEAM_TARGET_C` (overshoot OK; без `READY_HOLD_MS`, без ±`STEAM_READY_BAND_C`). У `STEAM_FLOW` падіння Tt **не** дає `HS1`/`SS0` і **не** паузить помпу (FR-026); авто-`HS1` лише з `STEAM_READY` при закритій ручці.

---

## Tunables

Усі температури, таймінги, потужності, PI, декальцинація, лінк і `FLOW_PULSES_PER_ML` — **лише** в секції Tunables на початку [`scripts/013_main_brew_logic.cpp`](../scripts/013_main_brew_logic.cpp).

Каталог констант і дефолти (таблиці Temperatures / Timing / Powers / PI / Decalc / Link):  
[`specs/001-main-brew-logic/contracts/tunable-constants.md`](../specs/001-main-brew-logic/contracts/tunable-constants.md).

Session-4 орієнтири: `COLD_FLUSH_MS=4000`, `COLD_FLUSH_PCT=100`, `STEAM_IDLE_EXIT_MS=0`, `HEATER_KP_HEAT`/`HEATER_KI_HEAT`, `HEAT_UP_OVERSHOOT_OK_C=5.0`.

Тюнінг стенду = правка Tunables (+ за потреби синхрон контракту), без «магічних» літералів у тілі FSM.

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

Дві Arduino Nano: панель [`011_button_panel_ui.cpp`](../scripts/011_button_panel_ui.cpp) + основна `013`. Прошивка панелі session 4 **не** змінюється.

| Зв’язок | Основна (`013`) | Панель (`011`) |
| --- | --- | --- |
| UART TX | `D8` | → `D5` (RX) |
| UART RX | `D9` | ← `D6` (TX) |
| GND | спільний | спільний |

Решта основної плати — як у `006` / [`thermoblock-pi-heater.md`](thermoblock-pi-heater.md):

- ТЕН SSR `D4`, диммер ZC `D2` / GATE `D3`, потенціометри `A0` (помпа) / `A1` (уставка еспресо)
- MAX6675 термоблок `CS=A2`, бачок `CS=D11` обов’язково `HIGH`
- OLED I2C `A4`/`A5`
- Флоуметр `D5`, ручка пари `D10`

Порядок заливки й сценарії S1–S7: [`quickstart.md`](../specs/001-main-brew-logic/quickstart.md).

> ⚠️ Шлейф панелі **не** підключати до штатного `J8`. GND лише між двома Nano, не зі штатною платою кавоварки.

---

## Відмінність від прикладу протоколу: `B3` @ `HS*` / `SS*`

У [`button-panel-protocol.md`](button-panel-protocol.md) ілюстративний сценарій показує: у стані «пар готовий» (`HS0`, LED3 постійно) повторне натискання «Пар» (`B3`) → основна шле `SS1` (старт подачі).

**У `013` / цій фічі (session 4) це не так:**

| Джерело | `B3` у `HS0` / `HS1` / `SS*` | Старт `SS1` | Вихід з пару (`LT1`) |
| --- | --- | --- | --- |
| Приклад у protocol | → `SS1` | від кнопки «Пар» | (залежить від прикладу) |
| `013` (FR-021/022) | → **ignore** (не cancel, не `SS1`) | відкриття **ручки пари** (або одразу після `HS0`, якщо ручка вже відкрита) | лише `B1` або `B2` → `coolOrReady()` |

Тобто коди кадрів ті самі (`B1`/`B2`/`B3`, `HS*`, `SS*`, `LT*`, `P11`/`P10` для flush), але семантика основної: `B3`@пар = ігнор; `SS1` лише від ручки; cancel = `B1`/`B2`. Фіксація override: [`contracts/panel-uart.md`](../specs/001-main-brew-logic/contracts/panel-uart.md) § Feature-specific overrides.

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

# Main Brew Logic (`scripts/013_main_brew_logic.cpp`)

Прошивка основної Arduino Nano: відтворення штатної поведінки DeLonghi EC685 (розігрів → еспресо 1/2 з преінфузією → пар → охолодження → нагадування декальцинації). База — ПІ/диммер/OLED з [`006_thermoblock_pi_heater.cpp`](../scripts/006_thermoblock_pi_heater.cpp); події кнопок і LED — через UART-панель за [`button-panel-protocol.md`](button-panel-protocol.md).

Специфікація фічі: [`specs/001-main-brew-logic/`](../specs/001-main-brew-logic/) (`spec.md`, `plan.md`, `contracts/`, `quickstart.md`).

| Пункт | Реалізація |
| --- | --- |
| Призначення | FSM режимів на основній платі + лінк до панелі `011` |
| База апаратки | як `006` (ТЕН `D4`, диммер, MAX6675, OLED, потенціометри) |
| Панель | `SoftwareSerial` на `D8`/`D9` @ `19200` (як `012`) |
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
| `D10` | ручка пари — заглушка | ручка пари (вхід для `STEAM_FLOW`) |
| решта (`D2`/`D3`/`D4`, `A0`/`A1`, термопари, OLED) | без змін щодо `006` | без змін |

> Спільний `GND` лише між двома Nano; шлейф панелі від’єднати від штатного `J8` — як у [`button-panel-ui.md`](button-panel-ui.md).

---

## FSM (коротко)

Стани `MachineState` (без `COLD_START`). Повна матриця кнопок і `coolOrReady()` — [`contracts/main-fsm.md`](../specs/001-main-brew-logic/contracts/main-fsm.md).

```
POWER_ON → HE1 → ESPRESSO_HEAT
                 │ ready(±READY_BAND_C, READY_HOLD_MS)
                 ▼
            ESPRESSO_READY (HE0)
                 ├─ B1 → BREW_1 (P11): PREINFUSE → PAUSE → FLOW; stop B1 → P10
                 ├─ B2 → BREW_2 (P21): те саме; stop B2 → P20
                 └─ B3 → STEAM_HEAT (HS1, target STEAM_TARGET_C)
                            ├─ |Tt−steam|≤STEAM_READY_BAND_C → STEAM_READY (HS0, без hold)
                            ├─ B3 → coolOrReady()
                            │         STEAM_READY:
                            │           handle open → STEAM_FLOW (SS1, імпульси; OLED OPEN)
                            │           idle STEAM_IDLE_EXIT_MS (>0) → STEAM_COOL (LT1)
                            │             // дефолт 0 = без авто-LT1
                            │           B3 → coolOrReady()  // НЕ SS1
                            │         STEAM_FLOW: handle close → SS0 → coolOrReady()
                            └─ STEAM_COOL: ready → LT0 → ESPRESSO_READY

FAULT ← невалідна Tt / SAFETY_CUTOFF_C  (лише power cycle)
```

Ключові правила:

- Під час `BREW_1` чужі кнопки (`B2`/`B3`) ігнор; під час `BREW_2` — `B1`/`B3`.
- У всіх парових/охолоджувальних станах `B1`/`B2` ігнор; `B3` ігнор у `STEAM_FLOW` / `STEAM_COOL`.
- Помпа: преінфузія `PREINFUSION_PCT`; FLOW — живий потенціометр `A0`; пар — імпульси `STEAM_PULSE_*`; інакше `PUMP_OFF_PCT`.
- `coolOrReady()`: ціль → уставка еспресо; якщо Tt вища — `LT1`/`STEAM_COOL`; інакше `LT0` і `HE1`/`ESPRESSO_HEAT` або `ESPRESSO_READY`.

---

## Tunables

Усі температури, таймінги, потужності, PI, декальцинація, лінк і `FLOW_PULSES_PER_ML` — **лише** в секції Tunables на початку [`scripts/013_main_brew_logic.cpp`](../scripts/013_main_brew_logic.cpp).

Каталог констант і дефолти (таблиці Temperatures / Timing / Powers / PI / Decalc / Link):  
[`specs/001-main-brew-logic/contracts/tunable-constants.md`](../specs/001-main-brew-logic/contracts/tunable-constants.md).

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

Дві Arduino Nano: панель [`011_button_panel_ui.cpp`](../scripts/011_button_panel_ui.cpp) + основна `013`.

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

Порядок заливки й сценарії S1–S6: [`quickstart.md`](../specs/001-main-brew-logic/quickstart.md).

> ⚠️ Шлейф панелі **не** підключати до штатного `J8`. GND лише між двома Nano, не зі штатною платою кавоварки.

---

## Відмінність від прикладу протоколу: `B3` @ `HS0`

У [`button-panel-protocol.md`](button-panel-protocol.md) ілюстративний сценарій показує: у стані «пар готовий» (`HS0`, LED3 постійно) повторне натискання «Пар» (`B3`) → основна шле `SS1` (старт подачі).

**У `013` / цій фічі це не так:**

| Джерело | `B3` у `HS0` (ручка закрита) | Старт `SS1` |
| --- | --- | --- |
| Приклад у protocol | → `SS1` | від кнопки «Пар» |
| `013` (FR-021) | → `coolOrReady()` одразу (без idle 3 с); **не** `SS1` | лише відкриття **ручки пари** |

Тобто коди кадрів ті самі (`B3`, `HS*`, `SS*`, `LT*`), але семантика основної плати для `B3`@`HS0` — скасування пару, не подача. Фіксація override: [`contracts/panel-uart.md`](../specs/001-main-brew-logic/contracts/panel-uart.md) § Feature-specific override.

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

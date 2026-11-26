# Contract: Tunable Constants

**Правило імплементації**: усі значущі цифрові значення параметрів, лімітів, градусів, таймінгів, потужностей і періодів опитування SHALL бути іменованими константами в одній секції на початку `scripts/013_main_brew_logic.cpp` (або підключеному `*_config` заголовку). У тілі FSM/loop заборонені «магічні» літерали для бізнес-логіки (допустимі лише `0`/`1` для булевої логіки, індекси циклів, розміри буферів протоколу).

Кожна константа має короткий коментар «навіщо» і посилання на FR/SC за потреби.

**Source of truth (T041 + clarify 2026-11-15)**: числові дефолти нижче = поточна секція Tunables у `scripts/013_main_brew_logic.cpp` (стенд-тюнінг), **окрім** значень, явно змінених у spec 2026-11-15 (`TARGET_*`, `STEAM_*_TARGET_C`, `PUMP_MIN/MAX_PCT`). ПІ Kp/Ki/FF **не відкочувати** під старі табличні 4×/2.5×.

## Temperatures (°C)

| Constant | Default | Used for |
| --- | --- | --- |
| `TARGET_MIN_C` | **85.0** | Низький край потенціометра еспресо (FR-030) |
| `TARGET_MAX_C` | **105.0** | Верхній край потенціометра еспресо (FR-030) |
| `TARGET_OFF_BAND_C` | 1.0 | Біля мінімуму = нагрів OFF |
| `STEAM_READY_TARGET_C` | **120.0** | Нагрів/`HS0` з закритою ручкою; критерій `HS0` = `Tt ≥` цієї константи (FR-032) |
| `STEAM_FLOW_TARGET_C` | **128.0** | Підтримка під час відкритої ручки / `STEAM_FLOW` (FR-032) |
| `STEAM_TARGET_C` | *(legacy alias)* | Якщо лишається в коді — MUST дорівнювати `STEAM_READY_TARGET_C`; не використовувати як ціль у `STEAM_FLOW` |
| `STEAM_READY_BAND_C` | 0.0 | **Не використовується** для `HS0` |
| `READY_BAND_C` | 1.0 | Допуск: `HE0` нижня; `LT0` верхня |
| `HEATER_DEADBAND_C` | 0.5 | Мертва зона ПІ |
| `HEAT_UP_OVERSHOOT_OK_C` | 5.0 | Документований допустимий короткочасний overshoot |
| `SAFETY_CUTOFF_C` | **145.0** (стенд) | Аварійне вимкнення; MUST бути строго вище `STEAM_FLOW_TARGET_C` |
| `TEMP_VALID_MIN_C` | 1.0 | Нижня правдоподібність термопари |
| `TEMP_VALID_MAX_C` | 200.0 | Верхня правдоподібність термопари |

## Timing (ms unless noted)

| Constant | Default | Used for |
| --- | --- | --- |
| `READY_HOLD_MS` | 1000 | Утримання перед `HE0` / `LT0` |
| `COLD_FLUSH_MS` | стенд (spec орієнтир 4000) | Тривалість холодного проливу (`B1`); OLED `N` від цього |
| `PREINFUSION_MS` | 2000 | Тривалість преінфузії (`B2`) |
| `PREINFUSION_PAUSE_MS` | 2000 | Пауза після преінфузії |
| `STEAM_PULSE_ON_MS` | стенд | Імпульс помпи пару / cool ON |
| `STEAM_PULSE_OFF_MS` | 880 | Імпульс помпи пару / cool OFF |
| `STEAM_IDLE_EXIT_MS` | 0 | Idle→`LT1` **вимкнено** |
| `HEATER_WINDOW_MS` | 1000 | Вікно часового ШІМ ТЕНу |
| `CONTROL_INTERVAL_MS` | 100 | Період перерахунку ПІ |
| `BLOCK_TEMP_INTERVAL_MS` | 250 | Опитування MAX6675 |
| `MAX6675_BOOT_DELAY_MS` | 500 | Перша конверсія MAX6675 після живлення |
| `MS_PER_S` | 1000 | SI: мс → с |
| `POT_INTERVAL_MS` | 20 | Опитування потенціометрів |
| `OLED_INTERVAL_MS` | 200 | Оновлення OLED (поза brew) |
| `BREW_DISPLAY_INTERVAL_MS` | 500 | Оновлення OLED heat/ready/brew/flush (SC-004/016/017; FR-008/033/034) |
| `SERIAL_INTERVAL_MS` | 500 | Періодичний USB-лог |
| `WATCHDOG_TIMEOUT_MS` | 2000 | WDT |
| `BUTTON_REACT_MAX_MS` | 300 | Цільовий SLA зупинки (тест) |
| `TIMING_TOLERANCE_MS` | 200 | Допуск стендових перевірок |

## Powers (%)

| Constant | Default | Used for |
| --- | --- | --- |
| `COLD_FLUSH_PCT` | 100 | Холодний пролив |
| `PREINFUSION_PCT` | стенд | Преінфузія еспресо |
| `STEAM_PULSE_PCT` | 100 | Імпульс пари / cool |
| `PUMP_OFF_PCT` | 0 | Явний «стоп» |
| `PUMP_MIN_PCT` | **50** | Низький край потенціометра помпи (FR-031) |
| `PUMP_MAX_PCT` | **100** | Верхній край потенціометра помпи (FR-031) |

## PI / pots / dimmer

Чотири профілі (FR-027 / SC-010): **heat-up** · **base** · **brew-flow** · **steam-flow** + FF під час flow. Без target-offset. Числові Kp/Ki/FF — **як у прошивці** (не відкочувати в цій ітерації).

| Constant | Default | Used for |
| --- | --- | --- |
| `HEATER_KP` / `HEATER_KI` | стенд у `013` | Базовий ПІ |
| `HEATER_KP_HEAT` / `HEATER_KI_HEAT` | стенд у `013` | Heat-up |
| `HEATER_KP_BREW` / `HEATER_KI_BREW` / `HEATER_FF_BREW_PCT` | стенд у `013` | Brew-flow |
| `HEATER_KP_STEAM` / `HEATER_KI_STEAM` / `HEATER_FF_STEAM_PCT` | стенд у `013` | Steam-flow |
| `POT_SAMPLES` | 8 | Усереднення АЦП |
| `POT_SMOOTHING` | 0.3 | EMA потенціометрів |
| `DIMMER_HYSTERESIS` | 2 | Гістерезис рівня диммера |
| `TEMP_INVALID_STREAK_LIMIT` | 3 | Підряд невалідних читань → fault |

## Decalc (accumulated uptime, no RTC)

| Constant | Default | Used for |
| --- | --- | --- |
| `DECALC_INTERVAL_DAYS` | 28 | Інтервал у «днях роботи» |
| `DECALC_INTERVAL_S` | days×86400 | Похідна: секунди **uptime** |
| `DECALC_EEPROM_MAGIC` | 0xEC85 | Валідація блоку EEPROM |
| `DECALC_SAVE_EVERY_S` | 60 | Період flush uptime в EEPROM |

## Link / buffers

| Constant | Default | Used for |
| --- | --- | --- |
| `LINK_BAUD` | 19200 | UART панелі |
| `PC_BAUD` | 115200 | USB Serial |
| `PANEL_RX_PIN` | 9 | |
| `PANEL_TX_PIN` | 8 | |
| `LINK_CODE_MAX_LEN` | 3 | |

## Flow display

| Constant | Default | Used for |
| --- | --- | --- |
| `FLOW_PULSES_PER_ML` | 0 | 0 = імпульси в логу; OLED brew більше не вимагає лічильник |

## OLED copy (cold flush)

| String | When |
| --- | --- |
| `Cold flush N s` | Default ASCII SSD1306 (FR-033) |
| `Холодний пролив N сек` | Лише якщо підключено кириличний шрифт |

`N = ceil(remainingMs / 1000)` у цілих секундах.

## OLED labels (shared dictionary)

Повний набір (ASCII). Short — якщо не вміщується по ширині 128 px (`textSize=1` ≈ 21 символ).

| Full | Short | Meaning | Screens |
| --- | --- | --- | --- |
| `T_current` | `T_now` | Поточна Tt (°C) | FR-008, FR-029, FR-034 |
| `T_target` | `T_tar` | Уставка температури (°C) | FR-008, FR-034 |
| `Heat_power` | `Heat` | Потужність ТЕНу (%) | FR-008, FR-029, FR-034 |
| `Pump_current` | `P_now` | Фактична команда помпи (%) | FR-008, FR-034 |
| `Pump_target` | `P_tar` | Уставка помпи з pot A0 (%) | **лише FR-034** |
| `Valve open` / `Valve close` | `V open` / `V close` | Стан ручки пари | FR-029 |

**Layout FR-034** (4 рядки): `T_current` → `T_target` → `Heat_power` → (`Pump_current` + `Pump_target`). Рядок помпи MAY завжди використовувати short.

**Заборонено** як labels: `Tt`, `S`, `H`, `Pp`, `Ps`, голі `open`/`close`.

## Compliance check (implementation)

Перед merge прошивки:

1. `rg` по скетчу: немає бізнес-літералів поза секцією констант / таблицею диммера для °C/%/мс/Kp (зокрема 85, 105, 120, 128, 50, 100 для pot/steam).
2. Таблиця `DIMMER_CURVE` лишається даними калібрування.
3. Зміна тюнінгу = правка лише секції Tunables (+ за потреби цей контракт).
4. `SAFETY_CUTOFF_C > STEAM_FLOW_TARGET_C`.
5. ПІ-коефіцієнти цієї ітерації не «виправляти» під застарілі табличні множники без стенду.

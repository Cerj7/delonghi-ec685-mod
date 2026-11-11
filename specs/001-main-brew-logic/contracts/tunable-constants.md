# Contract: Tunable Constants

**Правило імплементації**: усі значущі цифрові значення параметрів, лімітів, градусів, таймінгів, потужностей і періодів опитування SHALL бути іменованими константами в одній секції на початку `scripts/013_main_brew_logic.cpp` (або підключеному `*_config` заголовку). У тілі FSM/loop заборонені «магічні» літерали для бізнес-логіки (допустимі лише `0`/`1` для булевої логіки, індекси циклів, розміри буферів протоколу).

Кожна константа має короткий коментар «навіщо» і посилання на FR/SC за потреби.

## Temperatures (°C)

| Constant | Default | Used for |
| --- | --- | --- |
| `TARGET_MIN_C` | 20.0 | Низький край потенціометра еспресо |
| `TARGET_MAX_C` | 100.0 | Верхній край потенціометра еспресо |
| `TARGET_OFF_BAND_C` | 1.0 | Біля мінімуму = нагрів OFF |
| `STEAM_TARGET_C` | 120.0 | Цііль пару |
| `STEAM_READY_BAND_C` | 2.0 | ± смуга для миттєвого `HS0` (без `READY_HOLD_MS`; SC-005) |
| `READY_BAND_C` | 1.0 | ± допуск готовності еспресо / після `LT1` (з hold) |
| `HEATER_DEADBAND_C` | 0.5 | Мертва зона ПІ |
| `SAFETY_CUTOFF_C` | 130.0 | Аварійне вимкнення ТЕНу (≥ пару) |
| `TEMP_VALID_MIN_C` | 1.0 | Нижня правдоподібність термопари |
| `TEMP_VALID_MAX_C` | 200.0 | Верхня правдоподібність термопари |

## Timing (ms unless noted)

| Constant | Default | Used for |
| --- | --- | --- |
| `READY_HOLD_MS` | 1000 | Утримання в смузі перед `HE0`/`LT0` |
| `PREINFUSION_MS` | 2000 | Тривалість преінфузії |
| `PREINFUSION_PAUSE_MS` | 2000 | Пауза після преінфузії |
| `STEAM_PULSE_ON_MS` | 500 | Імпульс помпи пару ON |
| `STEAM_PULSE_OFF_MS` | 1000 | Імпульс помпи пару OFF |
| `STEAM_IDLE_EXIT_MS` | 0 | Idle у `HS0` → `LT1`; **0 = вимкнено** (лише ручка / `B3`). Ненульове — опційний авто-вихід |
| `HEATER_WINDOW_MS` | 1000 | Вікно часового ШІМ ТЕНу |
| `CONTROL_INTERVAL_MS` | 100 | Період перерахунку ПІ |
| `BLOCK_TEMP_INTERVAL_MS` | 250 | Опитування MAX6675 |
| `POT_INTERVAL_MS` | 20 | Опитування потенціометрів |
| `OLED_INTERVAL_MS` | 200 | Оновлення OLED (поза вимогою brew ≥2 Гц) |
| `BREW_DISPLAY_INTERVAL_MS` | 500 | Оновлення brew-дисплея (SC-004) |
| `SERIAL_INTERVAL_MS` | 500 | Періодичний USB-лог |
| `WATCHDOG_TIMEOUT_MS` | 2000 | WDT |
| `BUTTON_REACT_MAX_MS` | 300 | Цільовий SLA зупинки (тест, не обов’язково код) |
| `TIMING_TOLERANCE_MS` | 200 | Допуск стендових перевірок |

## Powers (%)

| Constant | Default | Used for |
| --- | --- | --- |
| `PREINFUSION_PCT` | 40 | Преінфузія |
| `STEAM_PULSE_PCT` | 100 | Імпульс пари |
| `PUMP_OFF_PCT` | 0 | Явний «стоп» |

## PI / pots / dimmer

| Constant | Default | Used for |
| --- | --- | --- |
| `HEATER_KP` | 5.0 | ПІ Kp |
| `HEATER_KI` | 0.05 | ПІ Ki |
| `POT_SAMPLES` | 8 | Усереднення АЦП |
| `POT_SMOOTHING` | 0.3 | EMA потенціометрів |
| `DIMMER_HYSTERESIS` | 2 | Гістерезис рівня диммера |
| `TEMP_INVALID_STREAK_LIMIT` | 3 | Підряд невалідних читань → fault |

## Decalc (accumulated uptime, no RTC)

| Constant | Default | Used for |
| --- | --- | --- |
| `DECALC_INTERVAL_DAYS` | 28 | Інтервал у «днях роботи» |
| `DECALC_INTERVAL_S` | days×86400 | Похідна: секунди **uptime**; для стенду можна зменшити |
| `DECALC_EEPROM_MAGIC` | (chosen) | Валідація блоку EEPROM |
| `DECALC_SAVE_EVERY_S` | 60 | Період flush uptime в EEPROM (допуск SC-008) |

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
| `FLOW_PULSES_PER_ML` | 0 | 0 = показувати імпульси; >0 = мл |

## Compliance check (implementation)

Перед merge прошивки:

1. `rg` по скетчу: немає літералів на кшталт `120`, `2000`, `40`, `500`, `1000`, `3000`, `28` у логіці поза секцією констант / таблиці диммера.
2. Таблиця `DIMMER_CURVE` лишається даними калібрування (допустимі літерали всередині масиву), але сама крива — іменована.
3. Зміна тюнінгу = правка лише секції Tunables (+ за потреби цей контракт).

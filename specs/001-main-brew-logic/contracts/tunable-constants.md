# Contract: Tunable Constants

**Правило імплементації**: усі значущі цифрові значення параметрів, лімітів, градусів, таймінгів, потужностей і періодів опитування SHALL бути іменованими константами в одній секції на початку `scripts/013_main_brew_logic.cpp` (або підключеному `*_config` заголовку). У тілі FSM/loop заборонені «магічні» літерали для бізнес-логіки (допустимі лише `0`/`1` для булевої логіки, індекси циклів, розміри буферів протоколу).

Кожна константа має короткий коментар «навіщо» і посилання на FR/SC за потреби.

**Source of truth (T041)**: дефолти нижче = поточна секція Tunables у `scripts/013_main_brew_logic.cpp` (стенд-тюнінг).

## Temperatures (°C)

| Constant | Default | Used for |
| --- | --- | --- |
| `TARGET_MIN_C` | 30.0 | Низький край потенціометра еспресо |
| `TARGET_MAX_C` | 110.0 | Верхній край потенціометра еспресо |
| `TARGET_OFF_BAND_C` | 1.0 | Біля мінімуму = нагрів OFF |
| `STEAM_TARGET_C` | 130.0 | Ціль пару; готовність `HS0` = `Tt ≥ STEAM_TARGET_C` (без верхньої межі) |
| `STEAM_READY_BAND_C` | 0.0 | **Не використовується** для `HS0` (залишено для сумісності/майбутнього гістерезису; критерій односторонній ≥ target) |
| `READY_BAND_C` | 1.0 | ± допуск готовності еспресо / після `LT1` (з hold) |
| `HEATER_DEADBAND_C` | 0.5 | Мертва зона ПІ |
| `HEAT_UP_OVERSHOOT_OK_C` | 5.0 | Документований допустимий короткочасний overshoot над ціллю під час/після heat-up (стенд; не окремий cutoff) |
| `SAFETY_CUTOFF_C` | 145.0 | Аварійне вимкнення ТЕНу (строго вище `STEAM_TARGET_C`; ≥ пару і heat-up overshoot) |
| `TEMP_VALID_MIN_C` | 1.0 | Нижня правдоподібність термопари |
| `TEMP_VALID_MAX_C` | 200.0 | Верхня правдоподібність термопари |

## Timing (ms unless noted)

| Constant | Default | Used for |
| --- | --- | --- |
| `READY_HOLD_MS` | 1000 | Утримання в смузі перед `HE0`/`LT0` |
| `COLD_FLUSH_MS` | 4000 | Тривалість холодного проливу (`B1`) |
| `PREINFUSION_MS` | 2000 | Тривалість преінфузії (`B2`) |
| `PREINFUSION_PAUSE_MS` | 2000 | Пауза після преінфузії |
| `STEAM_PULSE_ON_MS` | 500 | Імпульс помпи пару / cool ON |
| `STEAM_PULSE_OFF_MS` | 880 | Імпульс помпи пару / cool OFF |
| `STEAM_IDLE_EXIT_MS` | 0 | Idle→`LT1` **вимкнено** (session 4). `0 = вимкнено`. Залишено як Tunable на випадок стенду |
| `HEATER_WINDOW_MS` | 1000 | Вікно часового ШІМ ТЕНу |
| `CONTROL_INTERVAL_MS` | 100 | Період перерахунку ПІ |
| `BLOCK_TEMP_INTERVAL_MS` | 250 | Опитування MAX6675 |
| `MAX6675_BOOT_DELAY_MS` | 500 | Перша конверсія MAX6675 після живлення (`setup`) |
| `MS_PER_S` | 1000 | SI: мс → с (ПІ `dt`, decalc uptime) |
| `POT_INTERVAL_MS` | 20 | Опитування потенціометрів |
| `OLED_INTERVAL_MS` | 200 | Оновлення OLED (поза вимогою brew ≥2 Гц) |
| `BREW_DISPLAY_INTERVAL_MS` | 500 | Оновлення brew/flush-дисплея (SC-004) |
| `SERIAL_INTERVAL_MS` | 500 | Періодичний USB-лог |
| `WATCHDOG_TIMEOUT_MS` | 2000 | WDT |
| `BUTTON_REACT_MAX_MS` | 300 | Цільовий SLA зупинки (тест, не обов’язково код) |
| `TIMING_TOLERANCE_MS` | 200 | Допуск стендових перевірок |

## Powers (%)

| Constant | Default | Used for |
| --- | --- | --- |
| `COLD_FLUSH_PCT` | 100 | Холодний пролив |
| `PREINFUSION_PCT` | 80 | Преінфузія еспресо |
| `STEAM_PULSE_PCT` | 100 | Імпульс пари / cool |
| `PUMP_OFF_PCT` | 0 | Явний «стоп» |

## PI / pots / dimmer

Чотири профілі (FR-027 / SC-010): **heat-up** · **base** · **brew-flow** (~5× stand) · **steam-flow** (~4× stand) + FF під час flow. Без target-offset. Heat-up у `ESPRESSO_HEAT`/`STEAM_HEAT`; brew-flow лише `FLOW` у `BREW_2`; steam-flow — увесь `STEAM_FLOW`; cold flush / `STEAM_COOL` — ТЕН OFF. Стартові heat-up Kp/Ki — стенд (мають бути > base).

| Constant | Default | Used for |
| --- | --- | --- |
| `HEATER_KP` | 5.0 | Базовий ПІ Kp (idle / ready / preinfuse / pause / `STEAM_READY`) |
| `HEATER_KI` | 0.05 | Базовий ПІ Ki |
| `HEATER_KP_HEAT` | 12.0 | Heat-up Kp (стартово ~2.4× base; підкрутка на стенді) |
| `HEATER_KI_HEAT` | 0.12 | Heat-up Ki (стартово ~2.4× base; підкрутка на стенді) |
| `HEATER_KP_BREW` | 25.0 | Brew-flow Kp (~5× base, стенд) — лише `BrewPhase==FLOW` |
| `HEATER_KI_BREW` | 0.25 | Brew-flow Ki (~5× base, стенд) |
| `HEATER_FF_BREW_PCT` | 25 | Адитивний FF % під час brew `FLOW` (до виходу ПІ, clamp 0…100) |
| `HEATER_KP_STEAM` | 20.0 | Steam-flow Kp (~4× base, стенд) — увесь `STEAM_FLOW` |
| `HEATER_KI_STEAM` | 0.20 | Steam-flow Ki (~4× base, стенд) |
| `HEATER_FF_STEAM_PCT` | 20 | Адитивний FF % під час `STEAM_FLOW` (clamp 0…100) |
| `POT_SAMPLES` | 8 | Усереднення АЦП |
| `POT_SMOOTHING` | 0.3 | EMA потенціометрів |
| `DIMMER_HYSTERESIS` | 2 | Гістерезис рівня диммера |
| `TEMP_INVALID_STREAK_LIMIT` | 3 | Підряд невалідних читань → fault |

## Decalc (accumulated uptime, no RTC)

| Constant | Default | Used for |
| --- | --- | --- |
| `DECALC_INTERVAL_DAYS` | 28 | Інтервал у «днях роботи» |
| `DECALC_INTERVAL_S` | days×86400 | Похідна: секунди **uptime**; для стенду можна зменшити |
| `DECALC_EEPROM_MAGIC` | 0xEC85 | Валідація блоку EEPROM |
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

1. `rg` по скетчу: немає літералів на кшталт `130`, `145`, `2000`, `4000`, `80`, `500`, `880`, `28`, `30.0`, `110.0`, `25.0`, `20.0`, `0.25`, `0.20`, `0.12` у логіці поза секцією констант / таблиці диммера.
2. Таблиця `DIMMER_CURVE` лишається даними калібрування (допустимі літерали всередині масиву), але сама крива — іменована.
3. Зміна тюнінгу = правка лише секції Tunables (+ за потреби цей контракт).

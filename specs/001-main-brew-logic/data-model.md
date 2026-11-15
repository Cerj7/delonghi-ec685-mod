# Data Model: Main Brew Logic (EC685)

## Entities

### MachineState (FSM)

Фаза життєвого циклу основної плати.

| Value | Meaning | Panel indication (typical) |
| --- | --- | --- |
| `ESPRESSO_HEAT` | Початковий / повторний розігрів еспресо (також одразу після power-on); heat-up ПІ | `HE1`… |
| `ESPRESSO_READY` | Готовність до еспресо | після `HE0` або `LT0` |
| `COLD_FLUSH` | Холодний пролив / промив групи (`B1`); ТЕН OFF | `P11`…`P10` |
| `BREW_2` | Пролив 2 еспресо (преінфузія / пауза / пролив) | `P21`…`P20` |
| `STEAM_HEAT` | Нагрів до парової цілі; heat-up ПІ | `HS1`… |
| `STEAM_READY` | Пар готовий; утримання цілі `STEAM_TARGET_C`; ручка / авто-`HS1` (закрита ручка); вихід лише `B1`/`B2` | `HS0` |
| `STEAM_FLOW` | Подача пару (ручка відкрита); імпульси без паузи навіть при Tt < target; steam-flow ПІ | `SS1` |
| `STEAM_COOL` | Охолодження після `B1`/`B2` з пару; ТЕН OFF; ручка → імпульси; `B3` може повернути в пар | `LT1`…`LT0` |
| `FAULT` | Невалідна термопара / cutoff / аварія | дисплей аварії; ТЕН/помпа OFF |

> Примітка імплементації: колишній `BREW_1` **перейменовано** в `COLD_FLUSH` (поведінка більше не є «1 espresso preinfusion»).

**Transitions** (скорочено; повний контракт — [contracts/main-fsm.md](./contracts/main-fsm.md)):

- Power-on → `ESPRESSO_HEAT` (`HE1`) — окремого стану `COLD_START` немає
- `ESPRESSO_HEAT` + ready criterion → `ESPRESSO_READY` (`HE0`)
- `ESPRESSO_READY` + `B1` → `COLD_FLUSH` (`P11`); + `B2` → `BREW_2` (`P21`); + `B3` → `enterSteam()`
- `COLD_FLUSH` + timeout/`B1` → `ESPRESSO_READY` (`P10`)
- `BREW_2` + `B2` → `ESPRESSO_READY` (`P20`)
- `STEAM_HEAT` + `Tt ≥ STEAM_TARGET_C` → `STEAM_READY` (`HS0`, без hold); якщо ручка відкрита → одразу `STEAM_FLOW`; `B3` ignore; `B1`/`B2` → `coolOrReady()`
- `STEAM_READY` + handle open → `STEAM_FLOW`; `Tt < target` (ручка закрита) → `STEAM_HEAT`; `B1`/`B2` → `coolOrReady()`; `B3` ignore; **немає** idle→cool
- `STEAM_FLOW` + handle close → `SS0` → `STEAM_READY`; `Tt < target` → **залишитись** у flow; `B1`/`B2` → `coolOrReady()`; `B3` ignore
- `STEAM_COOL` + handle open → pulse pump (ТЕН OFF); ready criterion → pump 0 + `LT0` → `ESPRESSO_READY`; `B3` → `enterSteam()`; `B1`/`B2` block (no flush/brew)
- Any active + fault → `FAULT` (вихід v1: лише power cycle)

### TemperatureSetpoint

| Field | Type | Source | Notes |
| --- | --- | --- | --- |
| `espressoTargetC` | float | Pot `A1` | Шкала `TARGET_MIN_C`…`TARGET_MAX_C`; off-band біля мінімуму |
| `activeTargetC` | float | Derived | `espressoTargetC` або `STEAM_TARGET_C` (у cool — орієнтир для ready criterion; ТЕН OFF) |
| `mode` | enum | FSM | `ESPRESSO` \| `STEAM` \| `COOL` |

**Validation**: Невалідна Tt → нагрів заборонено. Cutoff `SAFETY_CUTOFF_C` → ТЕН OFF.

### ColdFlushProfile

| Field | Constant | Default |
| --- | --- | --- |
| durationMs | `COLD_FLUSH_MS` | 4000 |
| powerPct | `COLD_FLUSH_PCT` | 100 |
| heater | forced OFF | — |
| panel | `P11` / `P10` | early stop on repeat `B1` |

### BrewProfile

| Field | Constant | Default |
| --- | --- | --- |
| preinfusionMs | `PREINFUSION_MS` | 2000 |
| preinfusionPct | `PREINFUSION_PCT` | 40 |
| pauseMs | `PREINFUSION_PAUSE_MS` | 2000 |
| brewPct | pot `A0` | 0…100 live after pause |
| phase | enum | `PREINFUSE` \| `PAUSE` \| `FLOW` |
| heaterProfile | derived | `PREINFUSE`/`PAUSE` → base; `FLOW` → brew-flow |

Лише для `BREW_2` (не для `COLD_FLUSH`).

### PanelEvent / PanelCommand

| Direction | Codes |
| --- | --- |
| In (panel → main) | `B1`, `B2`, `B3` |
| Out (main → panel) | `HE1`/`HE0`, `P11`/`P10`, `P21`/`P20`, `HS1`/`HS0`, `SS1`/`SS0`, `LT1`/`LT0`, `DC1`/`DC0` |
| Service (USB → main) | `DC0` (обов’язково); опційно тестові `DC1`, `TIME`, зменшений інтервал |

Формат: ASCII ≤3 символи + `\n`, UART 19200 8N1. Прошивка панелі без змін: `LT1` = LED3 double-blink.

### WaterCounter

| Field | Type | Notes |
| --- | --- | --- |
| `pulses` | uint32 | Імпульси з `FLOWMETER_PIN` за поточний пролив |
| `displayValue` | derived | pulses, або мл якщо `FLOW_PULSES_PER_ML > 0` |
| `reset` | action | На старті `COLD_FLUSH` / `BREW_2` |

### SteamPulse

| Field | Constant | Default |
| --- | --- | --- |
| onMs | `STEAM_PULSE_ON_MS` | 110 |
| offMs | `STEAM_PULSE_OFF_MS` | 880 |
| powerPct | `STEAM_PULSE_PCT` | 100 |
| phase | enum | ON / OFF while handle open in `STEAM_FLOW` **або** `STEAM_COOL` |

### DecalcTimer

| Field | Storage | Notes |
| --- | --- | --- |
| `accumulatedOnSeconds` | EEPROM | Накопичений **uptime** під живленням (v1 без RTC / wall-clock) |
| `dcLatched` | EEPROM + RAM | `DC1` уже надіслано |
| `intervalSeconds` | const | `DECALC_INTERVAL_DAYS * 86400` с роботи (тюнінг: `DECALC_INTERVAL_S`) |
| `saveEverySeconds` | const | `DECALC_SAVE_EVERY_S` — період flush у EEPROM |

**Rules**:
- Після `accumulatedOnSeconds ≥ intervalSeconds` → один `DC1`, `dcLatched=true`
- USB `DC0` → панель `DC0`, `dcLatched=false`, `accumulatedOnSeconds=0` (і persist)
- Вимкнення живлення **не** збільшує акумулятор (очікувано для uptime-моделі)
- Не блокує еспресо / cold flush

### HeaterControl (from 006 + FR-027)

| Field | Notes |
| --- | --- |
| `tempBlockC`, `tempValid` | MAX6675 #1 |
| `heaterPercent`, integral, window | ПІ + time PWM |
| `activeKp`, `activeKi` | Вибір профілю: heat-up / base / brew-flow / steam-flow |
| `pumpCommandPct` | 0 поза cold flush / brew-2 / steam(+cool) pulse; інакше профіль / імпульс / pot |

### HeaterPiProfile

| Profile | Constants | Default Kp/Ki | Active when |
| --- | --- | --- | --- |
| heat-up | `HEATER_KP_HEAT` / `HEATER_KI_HEAT` | стенд (> base); overshoot ~+3…5 °C OK | `ESPRESSO_HEAT`, `STEAM_HEAT` (поки Tt нижче цілі / під час розігріву) |
| base | `HEATER_KP` / `HEATER_KI` | 5.0 / 0.05 | Idle, ready, brew PREINFUSE/PAUSE, `STEAM_READY` |
| brew-flow | `HEATER_KP_BREW` / `HEATER_KI_BREW` + `HEATER_FF_BREW_PCT` | 25.0 / 0.25 (~5× stand) + FF 25 % | `BREW_2` phase `FLOW` only |
| steam-flow | `HEATER_KP_STEAM` / `HEATER_KI_STEAM` + `HEATER_FF_STEAM_PCT` | 20.0 / 0.20 (~4× stand) + FF 20 % | Entire `STEAM_FLOW` |
| *(forced off)* | — | heaterPercent=0 | `COLD_FLUSH`, `STEAM_COOL`, `FAULT` |

**Rules**: brew-flow > steam-flow; heat-up > base; цілі без offset; FF лише під час brew-`FLOW` / `STEAM_FLOW`; `SAFETY_CUTOFF_C` спільний; критерії `HE0`/`HS0` не послаблюються heat-up.

## Validation Rules (cross-cutting)

1. Espresso heat ready (`HE0`): `Tt ≥ espressoTargetC − READY_BAND_C` continuously for `READY_HOLD_MS` (no upper bound).
2. Espresso cool ready (`LT0`): `Tt ≤ espressoTargetC + READY_BAND_C` continuously for `READY_HOLD_MS` (upper bound only).
3. Steam ready (`HS0`): `Tt ≥ STEAM_TARGET_C` → immediate (no `READY_HOLD_MS`, no upper band). In `STEAM_READY` with handle **closed**: if `Tt < STEAM_TARGET_C` → auto `STEAM_HEAT`; after recovery + handle already open → `SS1`. In `STEAM_FLOW`: Tt drop does **not** leave flow / pause pump.
4. During `ESPRESSO_HEAT`: ignore `B1`/`B2`/`B3`.
5. During `COLD_FLUSH`: only `B1` stops early; `B2`/`B3` ignored; heater forced OFF; duration `COLD_FLUSH_MS` @ 100 %.
6. During `BREW_2`: only `B2` stops; `B1`/`B3` ignored; brew-flow PI only in phase `FLOW`.
7. During `HS1`/`HS0`/`SS*`: `B1`/`B2` → `coolOrReady()` / `LT1`; `B3` ignored. No idle auto-exit (`STEAM_IDLE_EXIT_MS=0`).
8. During `STEAM_COOL`: heater OFF; open handle → steam pulse; cool-ready → pump 0 + `LT0` even if handle open; `B3` = `enterSteam()`; `B1`/`B2` do not start flush/brew.
9. Pump MUST be 0% outside `COLD_FLUSH`, brew-2 FLOW/preinfuse, and steam/cool pulse ON phases.
10. Fault forces heater OFF, pump 0, latch `FAULT` until **power cycle** (v1; no auto-recovery).
11. Heater PI profile switches within ≤ one `CONTROL_INTERVAL_MS` when entering/leaving heat-up / `FLOW` / `STEAM_FLOW`.

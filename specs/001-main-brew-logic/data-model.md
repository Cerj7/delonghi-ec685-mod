# Data Model: Main Brew Logic (EC685)

Оновлено після clarify 2026-11-15: dual steam targets, pot ranges, OLED layouts.

## Entities

### MachineState (FSM)

Фаза життєвого циклу основної плати.

| Value | Meaning | Panel indication (typical) |
| --- | --- | --- |
| `ESPRESSO_HEAT` | Початковий / повторний розігрів еспресо (також одразу після power-on); heat-up ПІ | `HE1`… |
| `ESPRESSO_READY` | Готовність до еспресо | після `HE0` або `LT0` |
| `COLD_FLUSH` | Холодний пролив / промив групи (`B1`); ТЕН OFF; OLED countdown | `P11`…`P10` |
| `BREW_2` | Пролив 2 еспресо (преінфузія / пауза / пролив); OLED brew metrics | `P21`…`P20` |
| `STEAM_HEAT` | Нагрів до `STEAM_READY_TARGET_C` (120 °C); heat-up ПІ | `HS1`… |
| `STEAM_READY` | Пар готовий; утримання **120 °C**; ручка / авто-`HS1` (закрита); вихід лише `B1`/`B2` | `HS0` |
| `STEAM_FLOW` | Подача пару (ручка відкрита); ціль **128 °C**; імпульси без паузи; steam-flow ПІ | `SS1` |
| `STEAM_COOL` | Охолодження після `B1`/`B2` з пару; ТЕН OFF; OLED steam layout; `B3` → пар | `LT1`…`LT0` |
| `FAULT` | Невалідна термопара / cutoff / аварія | дисплей аварії; ТЕН/помпа OFF |

> Примітка імплементації: колишній `BREW_1` **перейменовано** в `COLD_FLUSH`.

**Transitions** (скорочено; повний контракт — [contracts/main-fsm.md](./contracts/main-fsm.md)):

- Power-on → `ESPRESSO_HEAT` (`HE1`)
- `ESPRESSO_HEAT` + ready criterion → `ESPRESSO_READY` (`HE0`)
- `ESPRESSO_READY` + `B1` → `COLD_FLUSH` (`P11`); + `B2` → `BREW_2` (`P21`); + `B3` → `enterSteam()`
- `COLD_FLUSH` + timeout/`B1` → `ESPRESSO_READY` (`P10`)
- `BREW_2` + `B2` → `ESPRESSO_READY` (`P20`)
- `STEAM_HEAT` + `Tt ≥ STEAM_READY_TARGET_C` → `STEAM_READY` (`HS0`); якщо ручка відкрита → одразу `STEAM_FLOW` (ціль 128); `B1`/`B2` → `coolOrReady()`
- `STEAM_READY` + handle open → `STEAM_FLOW` (ціль 128); `Tt < 120` (ручка закрита) → `STEAM_HEAT`; `B1`/`B2` → `coolOrReady()`
- `STEAM_FLOW` + handle close → `SS0`, ціль 120 → `STEAM_READY`; Tt drop → **залишитись** у flow; `B1`/`B2` → `coolOrReady()`
- `STEAM_COOL` + handle open → pulse; ready → pump 0 + `LT0`; `B3` → `enterSteam()`
- Any active + fault → `FAULT` (вихід v1: лише power cycle)

### TemperatureSetpoint

| Field | Type | Source | Notes |
| --- | --- | --- | --- |
| `espressoTargetC` | float | Pot `A1` | Шкала `TARGET_MIN_C`…`TARGET_MAX_C` (**85…105 °C**); off-band біля мінімуму |
| `activeTargetC` | float | Derived | `espressoTargetC`, або `STEAM_READY_TARGET_C` (120), або `STEAM_FLOW_TARGET_C` (128); у cool — орієнтир еспресо (ТЕН OFF) |
| `mode` | enum | FSM | `ESPRESSO` \| `STEAM_READY` \| `STEAM_FLOW` \| `COOL` |

**Validation**: Невалідна Tt → нагрів заборонено. Cutoff `SAFETY_CUTOFF_C` (>128 °C) → ТЕН OFF.

### PumpSetpoint

| Field | Type | Source | Notes |
| --- | --- | --- | --- |
| `potPumpPercent` | uint8 | Pot `A0` | Мапінг на `PUMP_MIN_PCT`…`PUMP_MAX_PCT` (**50…100 %**) |
| `pumpCommandPct` | uint8 | FSM | 0 поза дозволеними контекстами; інакше flush / preinfuse / pot / pulse |

### ColdFlushProfile

| Field | Constant | Default |
| --- | --- | --- |
| durationMs | `COLD_FLUSH_MS` | 4000 (стенд може відрізнятись) |
| powerPct | `COLD_FLUSH_PCT` | 100 |
| heater | forced OFF | — |
| panel | `P11` / `P10` | early stop on repeat `B1` |
| oled | countdown | «Cold flush N s» (`N=ceil(remainingMs/1000)`) |

### BrewProfile

| Field | Constant | Default |
| --- | --- | --- |
| preinfusionMs | `PREINFUSION_MS` | 2000 |
| preinfusionPct | `PREINFUSION_PCT` | стенд (Tunables у прошивці) |
| pauseMs | `PREINFUSION_PAUSE_MS` | 2000 |
| brewPct | pot `A0` | `PUMP_MIN_PCT`…`PUMP_MAX_PCT` live after pause |
| phase | enum | `PREINFUSE` \| `PAUSE` \| `FLOW` |
| heaterProfile | derived | `PREINFUSE`/`PAUSE` → base; `FLOW` → brew-flow |
| oled | brew metrics | Tt / setpoint / heater% / pump% |

Лише для `BREW_2` (не для `COLD_FLUSH`).

### OledLayout

| Context | Fields shown |
| --- | --- |
| Idle / heat / ready (espresso) | Tt, heater%, setpoint (як `006`) |
| `BREW_2` | Tt, setpoint, heater%, pump% |
| `COLD_FLUSH` | «Cold flush N s» (або UA якщо шрифт дозволяє) |
| Steam (`HS*`/`SS*`) + `STEAM_COOL` | Tt, heater%, `open`\|`close` |
| `FAULT` | fault message |

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
| `pulses` | uint32 | Імпульси з `FLOWMETER_PIN` (внутрішня логіка / лог; **не** обов’язкове OLED-поле) |
| `displayValue` | derived | опційно мл якщо `FLOW_PULSES_PER_ML > 0` |
| `reset` | action | На старті `COLD_FLUSH` / `BREW_2` |

### SteamPulse

| Field | Constant | Default |
| --- | --- | --- |
| onMs | `STEAM_PULSE_ON_MS` | стенд (Tunables) |
| offMs | `STEAM_PULSE_OFF_MS` | 880 |
| powerPct | `STEAM_PULSE_PCT` | 100 |
| phase | enum | ON / OFF while handle open in `STEAM_FLOW` **або** `STEAM_COOL` |

### DecalcTimer

| Field | Storage | Notes |
| --- | --- | --- |
| `accumulatedOnSeconds` | EEPROM | Накопичений **uptime** під живленням (v1 без RTC / wall-clock) |
| `dcLatched` | EEPROM + RAM | `DC1` уже надіслано |
| `intervalSeconds` | const | `DECALC_INTERVAL_DAYS * 86400` с роботи |
| `saveEverySeconds` | const | `DECALC_SAVE_EVERY_S` |

**Rules**:
- Після `accumulatedOnSeconds ≥ intervalSeconds` → один `DC1`, `dcLatched=true`
- USB `DC0` → панель `DC0`, `dcLatched=false`, `accumulatedOnSeconds=0`
- Не блокує еспресо / cold flush

### HeaterControl (from 006 + FR-027)

| Field | Notes |
| --- | --- |
| `tempBlockC`, `tempValid` | MAX6675 #1 |
| `heaterPercent`, integral, window | ПІ + time PWM |
| `activeKp`, `activeKi` | Вибір профілю: heat-up / base / brew-flow / steam-flow |
| `pumpCommandPct` | 0 поза cold flush / brew-2 / steam(+cool) pulse |

### HeaterPiProfile

| Profile | Constants | Defaults | Active when |
| --- | --- | --- | --- |
| heat-up | `HEATER_KP_HEAT` / `HEATER_KI_HEAT` | **стенд у прошивці** (> base) | `ESPRESSO_HEAT`, `STEAM_HEAT` |
| base | `HEATER_KP` / `HEATER_KI` | стенд (типово 5.0 / 0.05) | Idle, ready, brew PREINFUSE/PAUSE, `STEAM_READY` |
| brew-flow | `HEATER_KP_BREW` / `HEATER_KI_BREW` + FF | **стенд у прошивці** | `BREW_2` phase `FLOW` only |
| steam-flow | `HEATER_KP_STEAM` / `HEATER_KI_STEAM` + FF | **стенд у прошивці** | Entire `STEAM_FLOW` |
| *(forced off)* | — | heaterPercent=0 | `COLD_FLUSH`, `STEAM_COOL`, `FAULT` |

**Rules**: brew-flow > steam-flow; heat-up > base; цілі без offset; FF лише під час brew-`FLOW` / `STEAM_FLOW`; `SAFETY_CUTOFF_C` спільний (>128); критерії `HE0`/`HS0` не послаблюються; **не відкочувати** стендові Kp/Ki/FF у цій ітерації.

## Validation Rules (cross-cutting)

1. Espresso heat ready (`HE0`): `Tt ≥ espressoTargetC − READY_BAND_C` continuously for `READY_HOLD_MS` (no upper bound).
2. Espresso cool ready (`LT0`): `Tt ≤ espressoTargetC + READY_BAND_C` continuously for `READY_HOLD_MS` (upper bound only).
3. Steam ready (`HS0`): `Tt ≥ STEAM_READY_TARGET_C` (120) → immediate. In `STEAM_READY` handle closed: `Tt < 120` → auto `STEAM_HEAT`. In `STEAM_FLOW`: target 128; Tt drop does **not** leave flow / pause pump.
4. During `ESPRESSO_HEAT`: ignore `B1`/`B2`/`B3`.
5. During `COLD_FLUSH`: only `B1` stops early; heater OFF; OLED countdown; duration `COLD_FLUSH_MS` @ 100 %.
6. During `BREW_2`: only `B2` stops; brew-flow PI only in phase `FLOW`; OLED brew metrics.
7. During `HS1`/`HS0`/`SS*`: `B1`/`B2` → `coolOrReady()`; `B3` ignored; no idle auto-exit.
8. During `STEAM_COOL`: heater OFF; open handle → pulse; cool-ready → pump 0 + `LT0`; OLED steam layout; `B3` = `enterSteam()`.
9. Pump MUST be 0% outside `COLD_FLUSH`, brew-2 FLOW/preinfuse, and steam/cool pulse ON phases.
10. Pot A1 → 85…105 °C; pot A0 → 50…100 %.
11. Fault forces heater OFF, pump 0, latch `FAULT` until **power cycle**.
12. Heater PI profile switches within ≤ one `CONTROL_INTERVAL_MS` when entering/leaving heat-up / `FLOW` / `STEAM_FLOW`.

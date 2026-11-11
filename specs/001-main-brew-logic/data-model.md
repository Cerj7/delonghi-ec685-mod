# Data Model: Main Brew Logic (EC685)

## Entities

### MachineState (FSM)

Фаза життєвого циклу основної плати.

| Value | Meaning | Panel indication (typical) |
| --- | --- | --- |
| `ESPRESSO_HEAT` | Початковий / повторний розігрів еспресо (також одразу після power-on) | `HE1`… |
| `ESPRESSO_READY` | Готовність до еспресо | після `HE0` або `LT0` |
| `BREW_1` | Пролив 1 еспресо (преінфузія / пауза / пролив) | `P11`…`P10` |
| `BREW_2` | Пролив 2 еспресо | `P21`…`P20` |
| `STEAM_HEAT` | Нагрів до парової цілі | `HS1`… |
| `STEAM_READY` | Пар готовий; утримання цілі `STEAM_TARGET_C`; idle / ручка / `B3`; авто-`HS1` лише з закритою ручкою | `HS0` |
| `STEAM_FLOW` | Подача пару (ручка відкрита); імпульси без паузи навіть при Tt < target; steam-flow ПІ | `SS1` |
| `STEAM_COOL` | Охолодження після idle/cancel; `B3` може повернути в пар | `LT1`…`LT0` |
| `FAULT` | Невалідна термопара / cutoff / аварія | дисплей аварії; ТЕН/помпа OFF |

**Transitions** (скорочено; повний контракт — [contracts/main-fsm.md](./contracts/main-fsm.md)):

- Power-on → `ESPRESSO_HEAT` (`HE1`) — окремого стану `COLD_START` немає
- `ESPRESSO_HEAT` + ready criterion → `ESPRESSO_READY` (`HE0`)
- `ESPRESSO_READY` + `B1`/`B2` → `BREW_*`; + `B3` → `enterSteam()` (`STEAM_HEAT` або одразу `STEAM_READY`)
- `BREW_*` + same button → `ESPRESSO_READY`
- `STEAM_HEAT` + `Tt ≥ STEAM_TARGET_C` → `STEAM_READY` (`HS0`, без hold); якщо ручка відкрита → одразу `STEAM_FLOW`; + `B3` → `coolOrReady()`
- `STEAM_READY` + handle open → `STEAM_FLOW`; `Tt < target` (ручка закрита) → `STEAM_HEAT`; idle `STEAM_IDLE_EXIT_MS` → `STEAM_COOL`; `B3` → `coolOrReady()`
- `STEAM_FLOW` + handle close → `SS0` → `STEAM_READY` (не cool); `Tt < target` → **залишитись** у `STEAM_FLOW` (без `HS1`/паузи помпи)
- `STEAM_COOL` + `B3` → `enterSteam()`; ready criterion → `LT0` → `ESPRESSO_READY`
- Any active + fault → `FAULT` (вихід v1: лише power cycle)

### TemperatureSetpoint

| Field | Type | Source | Notes |
| --- | --- | --- | --- |
| `espressoTargetC` | float | Pot `A1` | Шкала `TARGET_MIN_C`…`TARGET_MAX_C`; off-band біля мінімуму |
| `activeTargetC` | float | Derived | `espressoTargetC` або `STEAM_TARGET_C` |
| `mode` | enum | FSM | `ESPRESSO` \| `STEAM` |

**Validation**: Невалідна Tt → нагрів заборонено. Cutoff `SAFETY_CUTOFF_C` → ТЕН OFF.

### BrewProfile

| Field | Constant | Default |
| --- | --- | --- |
| preinfusionMs | `PREINFUSION_MS` | 2000 |
| preinfusionPct | `PREINFUSION_PCT` | 40 |
| pauseMs | `PREINFUSION_PAUSE_MS` | 2000 |
| brewPct | pot `A0` | 0…100 live after pause |
| phase | enum | `PREINFUSE` \| `PAUSE` \| `FLOW` |
| heaterProfile | derived | `PREINFUSE`/`PAUSE` → base; `FLOW` → brew-flow |

### PanelEvent / PanelCommand

| Direction | Codes |
| --- | --- |
| In (panel → main) | `B1`, `B2`, `B3` |
| Out (main → panel) | `HE1`/`HE0`, `P11`/`P10`, `P21`/`P20`, `HS1`/`HS0`, `SS1`/`SS0`, `LT1`/`LT0`, `DC1`/`DC0` |
| Service (USB → main) | `DC0` (обов’язково); опційно тестові `DC1`, `TIME`, зменшений інтервал |

Формат: ASCII ≤3 символи + `\n`, UART 19200 8N1.

### WaterCounter

| Field | Type | Notes |
| --- | --- | --- |
| `pulses` | uint32 | Імпульси з `FLOWMETER_PIN` за поточний пролив |
| `displayValue` | derived | pulses, або мл якщо `FLOW_PULSES_PER_ML > 0` |
| `reset` | action | На старті `BREW_1`/`BREW_2` |

### SteamPulse

| Field | Constant | Default |
| --- | --- | --- |
| onMs | `STEAM_PULSE_ON_MS` | 500 |
| offMs | `STEAM_PULSE_OFF_MS` | 1000 |
| powerPct | `STEAM_PULSE_PCT` | 100 |
| phase | enum | ON / OFF while handle open |

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
- Не блокує еспресо

### HeaterControl (from 006 + FR-027)

| Field | Notes |
| --- | --- |
| `tempBlockC`, `tempValid` | MAX6675 #1 |
| `heaterPercent`, integral, window | ПІ + time PWM |
| `activeKp`, `activeKi` | Вибір профілю: base / brew-flow / steam-flow (див. main-fsm) |
| `pumpCommandPct` | 0 поза brew/steam flow; інакше профіль / імпульс / pot |

### HeaterPiProfile

| Profile | Constants | Default Kp/Ki | Active when |
| --- | --- | --- | --- |
| base | `HEATER_KP` / `HEATER_KI` | 5.0 / 0.05 | Idle, heat, ready, cool, brew PREINFUSE/PAUSE, `STEAM_HEAT`/`STEAM_READY` |
| brew-flow | `HEATER_KP_BREW` / `HEATER_KI_BREW` + `HEATER_FF_BREW_PCT` | 20.0 / 0.20 (4×) + FF 20 % | Brew phase `FLOW` only |
| steam-flow | `HEATER_KP_STEAM` / `HEATER_KI_STEAM` + `HEATER_FF_STEAM_PCT` | 12.5 / 0.125 (2.5×) + FF 12 % | Entire `STEAM_FLOW` |

**Rules**: brew-flow > steam-flow > base за агресивністю; цілі без offset; FF лише під час brew-`FLOW` / `STEAM_FLOW` (адитив до виходу ПІ, clamp 0…100); `SAFETY_CUTOFF_C` спільний.

## Validation Rules (cross-cutting)

1. Espresso ready (`HE0`/`LT0`): `|Tt - espressoTargetC| ≤ READY_BAND_C` continuously for `READY_HOLD_MS`.
2. Steam ready (`HS0`): `Tt ≥ STEAM_TARGET_C` → immediate (no `READY_HOLD_MS`, no upper band). In `STEAM_READY` with handle **closed**: if `Tt < STEAM_TARGET_C` → auto `STEAM_HEAT`; after recovery + handle already open → `SS1`. In `STEAM_FLOW`: Tt drop does **not** leave flow / pause pump.
3. During `ESPRESSO_HEAT`: ignore `B1`/`B2`/`B3`.
4. During brew: only same-shot button stops; others ignored; brew-flow PI only in phase `FLOW`.
5. During steam/cool: ignore `B1`/`B2`; during `STEAM_COOL` `B3` = `enterSteam()` (not ignore).
6. Pump MUST be 0% outside brew flow and steam pulse ON; steam pulse continues even if Tt < target during `STEAM_FLOW`.
7. Fault forces heater OFF, pump 0, latch `FAULT` until **power cycle** (v1; no auto-recovery).
8. Idle: `STEAM_IDLE_EXIT_MS` (default 3000) with handle closed and no buttons → `LT1` / drop steam target. Handle close from flow returns to `STEAM_READY`, not `coolOrReady()`.
9. Heater PI profile switches within ≤ one `CONTROL_INTERVAL_MS` when entering/leaving `FLOW` or `STEAM_FLOW`.

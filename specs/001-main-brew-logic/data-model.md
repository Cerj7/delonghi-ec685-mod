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
| `STEAM_READY` | Пар готовий, очікування ручки / idle / `B3` | `HS0` |
| `STEAM_FLOW` | Подача пару (ручка відкрита) | `SS1` |
| `STEAM_COOL` | Охолодження після пару | `LT1`…`LT0` |
| `FAULT` | Невалідна термопара / cutoff / аварія | дисплей аварії; ТЕН/помпа OFF |

**Transitions** (скорочено; повний контракт — [contracts/main-fsm.md](./contracts/main-fsm.md)):

- Power-on → `ESPRESSO_HEAT` (`HE1`) — окремого стану `COLD_START` немає
- `ESPRESSO_HEAT` + ready criterion → `ESPRESSO_READY` (`HE0`)
- `ESPRESSO_READY` + `B1`/`B2` → `BREW_*`; + `B3` → `STEAM_HEAT`
- `BREW_*` + same button → `ESPRESSO_READY`
- `STEAM_HEAT` + `|Tt−STEAM_TARGET_C|≤STEAM_READY_BAND_C` → `STEAM_READY` (`HS0`, без hold); + `B3` → `coolOrReady()`
- `STEAM_READY` + handle open → `STEAM_FLOW`; idle/`B3` → `coolOrReady()` / `STEAM_COOL`
- `STEAM_FLOW` + handle close → `SS0` → `coolOrReady()` (не idle `HS0`)
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

### HeaterControl (from 006)

| Field | Notes |
| --- | --- |
| `tempBlockC`, `tempValid` | MAX6675 #1 |
| `heaterPercent`, integral, window | ПІ + time PWM |
| `pumpCommandPct` | 0 поза brew/steam flow; інакше профіль / імпульс / pot |

## Validation Rules (cross-cutting)

1. Espresso ready (`HE0`/`LT0`): `|Tt - espressoTargetC| ≤ READY_BAND_C` continuously for `READY_HOLD_MS`.
2. Steam ready (`HS0`): `|Tt - STEAM_TARGET_C| ≤ STEAM_READY_BAND_C` → immediate (no `READY_HOLD_MS`).
3. During `ESPRESSO_HEAT`: ignore `B1`/`B2`/`B3`.
4. During brew: only same-shot button stops; others ignored.
5. During steam/cool: ignore `B1`/`B2`.
6. Pump MUST be 0% outside brew flow and steam pulse ON.
7. Fault forces heater OFF, pump 0, latch `FAULT` until **power cycle** (v1; no auto-recovery).

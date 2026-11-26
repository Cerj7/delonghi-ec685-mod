# Data Model: Descaling Program (EC685)

Базові сутності brew/steam/`DecalcTimer` — у [../001-main-brew-logic/data-model.md](../001-main-brew-logic/data-model.md). Нижче — розширення для фічі `002`.

## Entities

### MachineState (розширення FSM)

Нові значення (поряд з існуючими `ESPRESSO_*` / `BREW_*` / `STEAM_*` / `FAULT`):

| Value | Meaning | Heater | Pump |
| --- | --- | --- | --- |
| `DESCALE_HOLD` | Утримання «пар»; опційний OLED-відлік після prompt | 0 %* | 0 % |
| `DESCALE_PREP` | Підказка: залити очисник, відкрити ручку, натиснути «пар» | 0 % | 0 % |
| `DESCALE_PHASE1` | Фаза очистки (імпульси + об’єм) | 0 % | імпульси phase1 |
| `DESCALE_REFILL` | Пауза доливу очисника (об’єм фази 1 збережено) | 0 % | 0 % |
| `DESCALE_WAIT_RINSE` | Підказка: залити чисту воду, відкрити ручку, «пар» | 0 % | 0 % |
| `DESCALE_PHASE2` | Фаза промивання | 0 % | імпульси phase2 |

\*Під час hold у `ESPRESSO_HEAT`/`READY` звичайний PI еспресо може лишатись до моменту входу в `DESCALE_PREP`; з `DESCALE_PREP` і далі ТЕН **завжди** 0 %. Практична реалізація MAY форсити 0 % уже в `DESCALE_HOLD` після prompt — безпечніше й простіше (рекомендовано).

**Вхід**: з не-блокувальних станів (`ESPRESSO_READY` / `ESPRESSO_HEAT`) після hold ≥ `DESCALE_HOLD_ENTER_MS`.

**Вихід success**: кінець `DESCALE_PHASE2` → ефект `DC0` → `ESPRESSO_READY` (або heat, якщо Tt далеко від уставки — без парової цілі).

**Вихід abort**: `FAULT` / power cycle → поза програмою, **без** `DC0`.

Повний контракт переходів: [contracts/descale-fsm.md](./contracts/descale-fsm.md).

### DescaleHoldGesture

| Field | Type | Notes |
| --- | --- | --- |
| `pressMs` | uint32 | `millis()` на `B3` |
| `active` | bool | Очікуємо `R3` / timeout enter |
| `countdownVisible` | bool | Після `DESCALE_HOLD_PROMPT_MS` |

**Rules**:
- `R3` і elapsed < prompt → short steam path (`001`)
- `R3` і prompt ≤ elapsed < enter → cancel, idle
- elapsed ≥ enter → `DESCALE_PREP` (навіть без `R3`; відпускання після входу не скасовує)

### DescaleSession

Сесія однієї програми (RAM only; не в EEPROM).

| Field | Type | Notes |
| --- | --- | --- |
| `phase` | enum | `NONE` \| `PHASE1` \| `PHASE2` |
| `phasePulses` | uint32 | Імпульси з початку поточної фази (refill **не** скидає) |
| `phaseVolumeMl` | derived | `phasePulses / FLOW_PULSES_PER_ML` |
| `pulsePhase` | enum | `ON` \| `OFF` |
| `pulsePhaseStartedMs` | uint32 | Старт поточного ON/OFF відрізка |
| `lastFlowPulseMs` | uint32 | Оновлюється з ISR/лічильника під час фази |
| `noFlowArmed` | bool | true лише під час ON |

**Validation**:
- Завершення фази 1 за об’ємом: `phaseVolumeMl ≥ DESCALE_PHASE_VOLUME_ML`
- Refill: no-flow під час ON ≥ `DESCALE_PHASE1_NOFLOW_MS` **і** volume < target
- Завершення фази 2: volume ≥ target **або** no-flow ≥ `DESCALE_PHASE2_NOFLOW_MS` під час ON

### DescalePulseProfile

| Field | Phase 1 const | Phase 2 const | Default |
| --- | --- | --- | --- |
| onMs | `DESCALE_PHASE1_ON_MS` | `DESCALE_PHASE2_ON_MS` | 500 / 500 |
| offMs | `DESCALE_PHASE1_OFF_MS` | `DESCALE_PHASE2_OFF_MS` | 1000 / 500 |
| noFlowMs | `DESCALE_PHASE1_NOFLOW_MS` | `DESCALE_PHASE2_NOFLOW_MS` | 1000 / 1500 |
| powerPct | `DESCALE_PUMP_PCT` | same | 100 |

### DescaleUiPrompt

| Id | Typical OLED text (UK) | State |
| --- | --- | --- |
| `COUNTDOWN` | «Режим декальцинації через N сек» | hold ≥ prompt |
| `PREP` | «Залийте 1 л очисника, відкрийте ручку та натисніть кнопку "Пар"» | `DESCALE_PREP` |
| `HANDLE_CLOSED` | Пояснення: відкрийте ручку пари | prep/wait + `B3` при закритій |
| `PHASE1_RUN` | «Виконується декальцинація: фаза очистки» | `DESCALE_PHASE1` |
| `REFILL` | «Долийте очисник і натисніть кнопку "Пар"» | `DESCALE_REFILL` |
| `WAIT_RINSE` | «Залийте 1 л води та натисніть кнопку "Пар"» | `DESCALE_WAIT_RINSE` |
| `PHASE2_RUN` | Виконання промивання | `DESCALE_PHASE2` |

Скорочення дозволені лише через ліміт екрана; зміст MUST лишитись однозначним (FR-017).

### DecalcTimer (reuse з `001`)

Без зміни layout EEPROM. Нове правило UI-шляху:

| Event | Effect |
| --- | --- |
| Успішний кінець фази 2 | Той самий, що USB `DC0`: панель `DC0`, `dcLatched=false`, `accumulatedOnSeconds=0`, save |
| Abort / power loss mid-program | Без змін таймера; якщо вже був latch — лишається |
| USB `DC0` | Як у `001` (аварійний/стендовий шлях) |

`dcLatched` / активність `DC1` **не** є умовою входу в програму (FR-001).

### PanelEvent (розширення)

| Direction | Codes (delta) |
| --- | --- |
| In | `B3` press (існуючий), **`R3`** release «пар» |
| Out | Без нових обов’язкових кодів; success → існуючий `DC0` |

Контракт: [contracts/panel-hold-uart.md](./contracts/panel-hold-uart.md).

## State transitions (overview)

```text
ESPRESSO_READY/HEAT --[B3 hold ≥ enter]--> DESCALE_PREP
DESCALE_PREP --[B3 + handle open]--> DESCALE_PHASE1
DESCALE_PHASE1 --[no-flow & vol < target]--> DESCALE_REFILL
DESCALE_REFILL --[B3 + handle open]--> DESCALE_PHASE1 (volume kept)
DESCALE_PHASE1 --[vol ≥ target]--> DESCALE_WAIT_RINSE
DESCALE_WAIT_RINSE --[B3 + handle open]--> DESCALE_PHASE2
DESCALE_PHASE2 --[vol ≥ target | no-flow]--> DC0 + ESPRESSO_READY
* --[FAULT]--> FAULT (no DC0)
```

## Relationships

- `DescaleSession` активна лише в `DESCALE_PHASE1` / `REFILL` / `WAIT_RINSE` / `PHASE2`.
- `DescaleHoldGesture` живе до входу в `DESCALE_PREP` або cancel через `R3`.
- `DecalcTimer` читається для індикації; пишеться clear лише з success path / USB.
- `WaterCounter` / flow ISR — спільне джерело імпульсів; сесія тримає власний `phasePulses` snapshot/base.

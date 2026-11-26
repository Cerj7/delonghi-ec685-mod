# Research: Descaling Program (EC685)

## 1. Детекція утримання «пар» (10 с)

**Decision**: Розширити UART: панель шле `B3` на press і `R3` на release кнопки «пар». Тривалість і рішення «короткий пар vs вхід у декальцинацію» рахує **лише основна** (`millis()`). Панель лишається «тупою» щодо режиму.

**Семантика на основній** (дозволені стани: не brew / не steam / не `FAULT` — типово `ESPRESSO_READY`, також `ESPRESSO_HEAT`):

| Подія | Дія |
| --- | --- |
| `B3` press | Старт hold-таймера; **не** викликати одразу `enterSteam()` |
| Утримання ≥ `DESCALE_HOLD_PROMPT_MS` (1 с) | OLED зворотний відлік секунд до входу |
| Утримання ≥ `DESCALE_HOLD_ENTER_MS` (10 с) | → `DESCALE_PREP`; помпа/ТЕН 0 % |
| `R3` і duration < prompt | Коротке натискання → існуюча логіка пару (`enterSteam()` / матриця `001`) |
| `R3` і prompt ≤ duration < enter | Скасувати відлік; **без** пару і **без** програми |
| `R3` після вже `DESCALE_PREP` | Ігнор як «відпускання входу» (підтвердження фаз — окремі короткі `B3`) |

**Rationale**: FR-001…003, FR-018; spec assumption про розширення протоколу; `LINK_CODE_MAX_LEN=3` дозволяє `R3`. Main лишається джерелом істини (конституція панелі з protocol).

**Alternatives considered**:
- Панель сама шле `H3`/`D3` після 10 с — відхилено: таймінги hold і countdown на OLED усе одно на main; панель починає «знати» сервісний режим.
- Keepalive/`B3` repeat — відхилено: шум UART, складніше.
- Лише delayed short-press без `R3` — неможливо надійно відрізнити hold від short без release.

## 2. Інтеграція з FSM `001` і steam short-press

**Decision**: Додати ортогональні стани декальцинації в той самий enum `MachineState` у `013`. Під час `DESCALE_*`: `B1`/`B2` ігнор; не слати `HS*`/`SS*`/`P*` brew/steam індикацію для фаз (щоб LED4/`DC1` міг лишатись видимим за правилами панелі). Вихід після фази 2 → `ESPRESSO_READY` / `coolOrReady`-шлях за температурою (ТЕН був 0 %, типово одразу ready або легкий heat).

**Rationale**: FR-015; SC-005; уникнути конфлікту LED3 «пар» з сервісом.

**Alternatives considered**: Окремий паралельний «режимний» прапорець поверх brew FSM — гірше для `pumpMayRun`/heater gates. Підрежим усередині `STEAM_*` — відхилено (ТЕН пару суперечить FR-006).

## 3. Скидання `DC1` після фази 2

**Decision**: Після успішного завершення фази 2 викликати той самий ефект, що `handleServiceDc0()`: кадр `DC0` на панель, `dcLatched=false`, `accumulatedOnSeconds=0`, persist EEPROM. USB `DC0` з `001` лишається чинним (FR-019). Переривання / power cycle / `FAULT` **не** викликають clear.

**Rationale**: FR-013/014; SC-005; один код шляху скидання.

**Alternatives considered**: Окремий soft-clear без EEPROM — відхилено (нагадування повернулось би). Скидати після фази 1 — відхилено spec.

## 4. Імпульси помпи, ТЕН, ручка

**Decision**:
- У всіх `DESCALE_*` примусово `heater = 0 %` (обійти PI / steam-flow профілі).
- Розширити `pumpMayRun()` на активні фази 1/2; поза ON-імпульсом і в prep/wait — 0 %.
- Імпульси: окремі Tunables (`DESCALE_PHASE1_*`, `DESCALE_PHASE2_*`), не reuse `STEAM_PULSE_*` (інші дефолти фази 2 і семантика).
- Потужність імпульсу: `DESCALE_PUMP_PCT = 100` (не потенціометр).
- Старт фази 1/2: лише якщо `isSteamHandleOpen()`; інакше OLED «відкрийте ручку».
- Закриття ручки **під час** фази не завершує фазу і не паузить імпульси (FR US4) — гідравліка сама змінює шлях.

**Rationale**: FR-005…008, FR-006; SC-002/003/006.

**Alternatives considered**: Пауза помпи при закритій ручці — відхилено spec. Соленоїд/програмний шлях — немає на EC685.

## 5. Об’єм і no-flow

**Decision**:
- Накопичувати `phasePulses` з існуючого ISR; мл = `pulses / FLOW_PULSES_PER_ML`.
- Для критерію `DESCALE_PHASE_VOLUME_ML` **вимагається** `FLOW_PULSES_PER_ML > 0` (калібрування на стенді як частина імплементації/quickstart; якщо 0 — стенд використовує тимчасове калібрувальне значення, не «магічний» обхід у логіці).
- No-flow: під час інтервалу ON помпи, якщо `(now - lastPulseMs) ≥ DESCALE_PHASEn_NOFLOW_MS` → подія. Під час OFF таймер no-flow **не** накопичується (скидається/паузиться на вході в OFF).
- Фаза 1 + no-flow + volume < target → `DESCALE_REFILL` (зберегти volume).
- Фаза 1 + volume ≥ target (або no-flow при вже ≥ target) → `DESCALE_WAIT_RINSE` (пріоритет завершення, не refill).
- Фаза 2 + volume ≥ target **або** no-flow ≥ timeout → success → `DC0`.

**Rationale**: FR-009…012; assumptions spec; `FLOW_PULSES_PER_ML` уже в Tunables `001`.

**Alternatives considered**: Окремий цільовий лічильник лише в імпульсах без мл — гірше для SC/текстів «1 л». No-flow у тому числі під час OFF — дає хибні «порожній бак».

## 6. OLED-тексти

**Decision**: Статичні рядки українською у Flash (`F()` / PROGMEM), формулювання зі spec; скорочення лише якщо не вміщується на SSD1306 128×64, зміст зберігається. Оновлення з існуючим OLED-інтервалом; під час countdown — частіше/на зміну секунди.

**Rationale**: FR-017; Memory Efficiency (немає динамічних `String`).

**Alternatives considered**: Тексти на панелі — немає дисплея користувача на панелі кнопок.

## 7. Персистентність фази / cancel

**Decision**: Power cycle під час програми → програма **не** відновлюється; `DC1` лишається якщо фаза 2 не завершена. Окремого cancel-жесту в v1 немає (spec assumptions).

**Rationale**: FR edge cases; YAGNI.

**Alternatives considered**: EEPROM checkpoint фази — відхилено (складність, ризик «застрягти» в кислоті після ребуту без UI).

## 8. Обсяг змін у репозиторії

**Decision**: Delta в `011_button_panel_ui.cpp` (`R3`), `013_main_brew_logic.cpp` (логіка), `docs/button-panel-protocol.md`, `docs/main-brew-logic.md`. Контракти фічі — у `specs/002-descaling-program/contracts/`. Точкове посилання з `001` contracts (не обов’язковий rewrite усієї `001`).

**Rationale**: Мінімальний blast radius; clear feature boundary.

**Alternatives considered**: Новий скетч `014_*` — відхилено (подвійне обслуговування brew+descale).

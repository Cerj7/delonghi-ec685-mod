# Research: Main Brew Logic (EC685)

## 1. База прошивки

**Decision**: Нова прошивка `scripts/013_main_brew_logic.cpp` як еволюція `006_thermoblock_pi_heater.cpp` + UART-лінк з `012_main_panel_link.cpp`.

**Rationale**: `006` уже має ПІ, часове ШІМ ТЕНу, диммер помпи, OLED, watchdog, захист термопари. `012` перевірив кадри `B*` / `HE*` / `P*` / `HS*` / `SS*` / `LT*` / `DC*` на реальному лінку D8/D9 @ 19200.

**Alternatives considered**:
- Окремі бібліотеки/модулі `.h` — відхилено для v1 (YAGNI, flash, стиль репо = моноскетчі; конституція IV — виняток у plan Complexity Tracking).
- Форк лише `012` без ПІ — не покриває нагрів/безпеку.

## 2. Конфлікт пінів D8/D9

**Decision**: D8 = TX → панель, D9 = RX ← панель (`SoftwareSerial`), як у протоколі. Штатне сканування кнопок на основній **не** використовується (події лише з панелі).

**Rationale**: Spec assumption + `button-panel-protocol.md`. У `006` D8/D9 були «не використовується / INPUT_PULLUP» — звільнені під лінк.

**Alternatives considered**: Hardware Serial на D0/D1 — конфлікт з USB debug; інший набір пінів — ламає вже змонтований стенд панелі.

## 3. Критерій готовності еспресо (`HE0` / `LT0`)

**Decision**: Температура в межах `READY_BAND_C` (±1 °C) від уставки протягом `READY_HOLD_MS` (1000 мс) безперервно. Той самий критерій після `LT1` перед `LT0`.

**Rationale**: Зафіксовано в clarify session і FR-002 / FR-014.

**Alternatives considered**: Миттєве «перетнуло уставку» — відхилено (шум MAX6675 0.25 °C, хибні готовності).

## 4. Захисний cutoff і пар 120 °C

**Decision**: Підняти `SAFETY_CUTOFF_C` вище парової цілі (наприклад 130 °C) окремою константою; еспресо-уставка лишається з потенціометра (шкала як у `006`: 20…100 °C). Парова ціль — `STEAM_TARGET_C = 120`.

**Rationale**: У `006` cutoff 105 °C блокував би пар. Spec вимагає утримання ~120 °C і захист від небезпечного перегріву.

**Alternatives considered**: Два різні cutoff залежно від режиму — можливо пізніше; для v1 достатньо одного підвищеного ліміту з коментарем.

## 5. Профіль проливу

**Decision**: `PREINFUSION_MS=2000`, `PREINFUSION_PCT=40`, `PREINFUSION_PAUSE_MS=2000`, далі `pumpPercent` з потенціометра до повторної тієї ж кнопки. Зупинка «чужою» кнопкою заборонена.

**Rationale**: FR-004…007, SC-002/003.

**Alternatives considered**: Автостоп за об’ємом — out of scope (ручна зупинка).

## 6. Пар: idle, ручка, імпульси, `HS0`

**Decision** (clarify session 2 + **session 3**, 2026-11-11 — supersedes earlier idle=0 / close→cool / ±2 band / flow-pause-on-cold):
- Готовність пару: **`Tt ≥ STEAM_TARGET_C`** (120 °C). Без `READY_HOLD_MS`, без симетричної смуги 118…122; Tt > 122 °C — валідне `HS0`. `STEAM_READY_BAND_C` не використовується для критерію.
- Після `HS0` машина тримає ціль `STEAM_TARGET_C` до idle→`LT1` або cancel.
- Падіння `Tt < STEAM_TARGET_C` у **`STEAM_READY` (ручка закрита)** → авто `HS1` (не `coolOrReady`); після знову `≥ target` → `HS0`; якщо ручка вже відкрита на `HS0` — одразу `SS1`.
- Падіння `Tt < STEAM_TARGET_C` у **`STEAM_FLOW`** → **залишитись** у flow: імпульси без паузи, **без** `HS1`/`SS0`; ТЕН гріє steam-flow профілем (session 3).
- Подача від ручки: `SS1` / імпульси `500`/`1000` @ 100 %; закриття → `SS0`, помпа 0 → **назад у `STEAM_READY`** (не миттєвий cool); стартує/продовжує idle.
- Idle **`STEAM_IDLE_EXIT_MS=3000`**: ручка закрита + немає кнопок → `LT1`, ціль пару знімається. Відкрита ручка скидає/паузить таймер. `0` лишається опцією Tunable (вимкнути авто-вихід).
- `B3` у `HS0`/`HS1` → `coolOrReady()` (не `SS1`); `B3` у `STEAM_COOL` → `enterSteam()` (`HS0`/`HS1`); `B1`/`B2` ігнор у парових/cool фазах.
- Вхід з еспресо-готовності: якщо вже `Tt ≥ target` — одразу `HS0`, інакше `HS1`.
- OLED: `OPEN` при відкритій ручці; мітки `HS`/`rdy`/`cool`.
- `FAULT` → ТЕН OFF, помпа 0; вихід v1 лише power cycle.

**Rationale**: Spec FR-009…015, FR-021/022/024–027, SC-005…010; користувач: під час відкритої ручки помпа має працювати імпульсно навіть нижче 120 °C; авто-`HS1` лише з закритою ручкою.

**Alternatives considered**: `B3` як старт подачі — відхилено; hold перед `HS0` — відхилено; симетрична ±2 °C — відхилено; idle=0 — скасовано session 2; миттєвий cool на закриття — відхилено; «липке» `HS0` — відхилено; пауза помпи/`HS1` під час `STEAM_FLOW` при Tt < target — **відхилено session 3** (ламає збивання молока).

## 6b. Агресивні ПІ-профілі під час проливу / пару

**Decision** (clarify session 3 + stand retune option **C**, 2026-11-11):
- Три tunable профілі: **base** (`HEATER_KP`/`HEATER_KI` = 5.0/0.05), **brew-flow** (4× → 20.0/0.20 + `HEATER_FF_BREW_PCT=20`), **steam-flow** (2.5× → 12.5/0.125 + `HEATER_FF_STEAM_PCT=12`).
- Brew-flow активний **лише** у фазі основного проливу (`FLOW` після паузи); преінфузія/пауза = base (FF=0).
- Steam-flow активний увесь час `STEAM_FLOW` (+ FF).
- Цілі без offset; FF — невеликий адитивний % до виходу ПІ під час flow; brew > steam > base; `SAFETY_CUTOFF_C` спільний.
- Стенд: option B (3×/2× без FF) **недостатньо** → C.

**Rationale**: FR-027 / SC-010; еспресо проливає більший об’єм води; практичний тест показав просідання при 3×/2× без FF.

**Alternatives considered**: Глобальне підняття Kp/Ki — відхилено; target-offset — відхилено; bang-bang — відхилено; brew на весь `BREW_*` — відхилено; brew=steam — відхилено; **B: 3×/2× без FF** — відхилено після стенду (мало).

## 7. Декальцинація (EEPROM, uptime)

**Decision (final, accepted)**: Без RTC. У EEPROM зберігати `accumulatedOnSeconds` (оновлювати кожні `DECALC_SAVE_EVERY_S`) і `dcLatched` (+ magic). Інтервал = `DECALC_INTERVAL_DAYS × 86400` с **накопиченого uptime** (час роботи під живленням). Після інтервалу — один `DC1`. Скидання: USB `DC0` → кадр `DC0` на панель + обнулення акумулятора / нова мітка. Стенд: зменшити `DECALC_INTERVAL_S` або force `DC1`.

**Rationale**: Підтверджено користувачем 2026-11-11: uptime без RTC — нормально. FR-016/023, SC-008 (допуск ≈ період save). Переживає вимкнення: акумулятор лишається в EEPROM (не тікає «календарний» час, поки машина вимкнена — це очікувано).

**Alternatives considered**:
- Wall-clock календар / RTC / Serial `TIME` — відхилено для v1 (зайве залізо; out of scope).
- Лічильник порцій — відхилено.

## 8. Дисплей і флоуметр

**Decision**: Поза проливом — як `006` (Tt, потужність ТЕНу %, уставка). Під час проливу — пріоритет: імпульси/об’єм флоуметра, Tt, потужність помпи %. Одиниця: імпульси, якщо калібрування мл ще немає (`FLOW_PULSES_PER_ML` константа; якщо 0 — показувати імпульси).

**Rationale**: FR-008, assumptions у spec.

**Alternatives considered**: Завжди мл — потребує калібрування, якого може не бути.

## 9. Іменовані константи (вимога замовника)

**Decision**: Усі значущі цифрові значення параметрів, лімітів, градусів, таймінгів, потужностей і інтервалів — лише через іменовані `const` / `constexpr` / `#define` на початку файлу (секція «Tunables»). У тілі FSM/loop — нуль «магічних» літералів для бізнес-параметрів. Каталог: [contracts/tunable-constants.md](./contracts/tunable-constants.md).

**Rationale**: Швидке редагування без пошуку по коду; узгоджено з тюнінгом ПІ в `006`.

**Alternatives considered**: `Config.h` окремим файлом — опційно пізніше; v1 — одна секція в скетчі.

## 10. Тестування

**Decision**: Ручні сценарії quickstart на стенді двох Nano + USB Serial; для DC — тимчасово зменшити `DECALC_INTERVAL_*` або сервісні команди `DC1`/`DC0`.

**Rationale**: Немає unit-test інфраструктури в репо для AVR.

**Alternatives considered**: native host mocks — out of scope цієї фічі.

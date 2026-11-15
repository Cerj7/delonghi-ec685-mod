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

**Decision** (оновлено 2026-11-13): **`HE0`** — `Tt ≥ espressoTargetC − READY_BAND_C` протягом `READY_HOLD_MS` (без верхньої межі; overshoot OK). **`LT0`** — окремо: `Tt ≤ espressoTargetC + READY_BAND_C` протягом `READY_HOLD_MS` (інакше після пару одразу «готово»).

**Rationale**: Зафіксовано в clarify session і FR-002 / FR-014.

**Alternatives considered**: Миттєве «перетнуло уставку» — відхилено (шум MAX6675 0.25 °C, хибні готовності).

## 4. Захисний cutoff і пар 120 °C

**Decision**: Підняти `SAFETY_CUTOFF_C` вище парової цілі (наприклад 130 °C) окремою константою; еспресо-уставка лишається з потенціометра (шкала як у `006`: 20…100 °C). Парова ціль — `STEAM_TARGET_C = 120`.

**Rationale**: У `006` cutoff 105 °C блокував би пар. Spec вимагає утримання ~120 °C і захист від небезпечного перегріву.

**Alternatives considered**: Два різні cutoff залежно від режиму — можливо пізніше; для v1 достатньо одного підвищеного ліміту з коментарем.

## 5. Пролив: холодний пролив (`B1`) + еспресо (`B2`)

**Decision** (clarify session 4, 2026-11-12 — supersedes «обидві кнопки = еспресо з преінфузією»):
- **`B1` / `COLD_FLUSH`**: `P11` → помпа `COLD_FLUSH_MS=4000` @ `COLD_FLUSH_PCT=100`, **ТЕН OFF** → `P10` (або достроковий `B1` → `P10`). Індикація через наявні `P11`/`P10` (панель без змін).
- Блок старту cold flush: активні `P21` / `HS1` / `SS1` / `LT1` (під час `HS0`/`SS0` `B1` веде в `LT1`, не в flush).
- **`B2` / `BREW_2`**: єдине еспресо — `PREINFUSION_MS=2000` @ 40 % → пауза 2000 → pot power до повторного `B2`. Зупинка «чужою» кнопкою заборонена; `B1` під час `P21` ігнорується.

**Rationale**: FR-004…007, SC-002/003; промив групи без нагріву; `B1` більше не дублює `B2`.

**Alternatives considered**: Автостоп за об’ємом — out of scope; окремий код панелі для flush — відхилено (панель зараз не міняємо); cold flush без `P11` — відхилено (користувач обрав A).

## 6. Пар: ручка, імпульси, вихід у `LT1`

**Decision** (session 2–3 + **session 4**, 2026-11-12 — supersedes idle→`LT1` і `B3`-cancel):
- Готовність пару: **`Tt ≥ STEAM_TARGET_C`** (120 °C). Без `READY_HOLD_MS`, без симетричної смуги 118…122; Tt > 122 °C — валідне `HS0`.
- Після `HS0` машина тримає ціль `STEAM_TARGET_C`, **доки** не надійде `B1` або `B2` → `LT1` / `coolOrReady()`.
- Падіння `Tt < STEAM_TARGET_C` у **`STEAM_READY` (ручка закрита)** → авто `HS1`; після знову `≥ target` → `HS0`; якщо ручка вже відкрита на `HS0` — одразу `SS1`.
- Падіння `Tt < STEAM_TARGET_C` у **`STEAM_FLOW`** → залишитись у flow: імпульси без паузи, **без** `HS1`/`SS0`; ТЕН — steam-flow.
- Подача від ручки: `SS1` / імпульси `110`/`880` @ 100 %; закриття → `SS0`, помпа 0 → **назад у `STEAM_READY`** (не миттєвий cool).
- **`STEAM_IDLE_EXIT_MS=0`**: idle-автовихід **вимкнено** назавжди для цієї фічі (session 4 option B).
- **`B3` у `HS1`/`HS0`/`SS*` ігнорується** (не cancel). `B3` у `STEAM_COOL` → `enterSteam()`.
- **`B1`/`B2` у `HS1`/`HS0`/`SS1`/`SS0`** → єдиний вихід у `LT1`.
- Індикація `LT1`: чинний протокол (LED3 double-blink); LED1+LED2 **не** вимагаємо (панель без змін).
- **`STEAM_COOL` (FR-028)**: ТЕН OFF; відкрита ручка → ті самі імпульси; ready criterion еспресо → помпа 0 навіть якщо ручка відкрита → `LT0`.
- OLED: `OPEN` при відкритій ручці; мітки `HS`/`rdy`/`cool`.
- `FAULT` → ТЕН OFF, помпа 0; вихід v1 лише power cycle.

**Rationale**: FR-009…015, FR-021/022/024–028, SC-005…011; користувач: вихід з пару лише кнопками еспресо; активне охолодження ручкою.

**Alternatives considered**: `B3`-cancel / idle→`LT1` — **відхилено session 4**; LED1+LED2 для `LT1` — відхилено (немає можливості міняти панель зараз); пауза помпи в `STEAM_FLOW` при холодному Tt — відхилено session 3; ПІ до уставки в cool — відхилено (обрано ТЕН OFF).

## 6b. ПІ-профілі: heat-up + brew/steam flow

**Decision** (session 3 option C + **session 4** heat-up):
- **Чотири** tunable профілі: **heat-up** (`HEATER_KP_HEAT`/`HEATER_KI_HEAT`, > base) у `ESPRESSO_HEAT` / `STEAM_HEAT`; **base** (5.0/0.05) idle/ready/preinfuse/pause; **brew-flow** (4× + FF) лише `FLOW` у `BREW_2`; **steam-flow** (2.5× + FF) увесь `STEAM_FLOW`.
- Cold flush і `STEAM_COOL` — ТЕН примусово OFF (профіль не застосовується).
- Цілі без offset; критерії `HE0`/`HS0` **без змін**; короткочасний overshoot ~+3…5 °C OK (нижче `SAFETY_CUTOFF_C`); heat-up Kp/Ki — стендовий тюнінг.
- Стенд session 3: 3×/2× без FF мало → C (4×/2.5× + FF) для flow.

**Rationale**: FR-027 / SC-010/012; швидший розігрів без послаблення готовності; користувач допустив невеликий overshoot.

**Alternatives considered**: Лише підняти base Kp — відхилено; послабити `HE0` до `Tt ≥ target` — відхилено; target-offset — відхилено; зміна панелі під нову індикацію — відхилено.

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

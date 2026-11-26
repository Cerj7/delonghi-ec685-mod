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

## 4. Захисний cutoff, діапазони pot і дві цілі пару

**Decision** (оновлено 2026-11-15):
- Еспресо-уставка з потенціометра A1: `TARGET_MIN_C`…`TARGET_MAX_C` = **85…105 °C**.
- Потужність помпи з A0: `PUMP_MIN_PCT`…`PUMP_MAX_PCT` = **50…100 %**.
- Пар — **дві** константи: `STEAM_READY_TARGET_C = 120` (нагрів/`HS0`, ручка закрита); `STEAM_FLOW_TARGET_C = 128` (ручка відкрита). Єдиний `STEAM_TARGET_C` deprecated як «обидві ролі».
- `SAFETY_CUTOFF_C` строго вище 128 °C (у прошивці стенд: 145 °C).

**Rationale**: Clarify 2026-11-15 / FR-030…032 / FR-018. У `006` cutoff ~105 °C блокував би пар на 128 °C.

**Alternatives considered**: Одна парова ціль 120 або 130 — відхилено (користувач: 120 готовність, 128 під час подачі); два різні cutoff за режимом — YAGNI для v1.

## 5. Пролив: холодний пролив (`B1`) + еспресо (`B2`)

**Decision** (clarify session 4, 2026-11-12 — supersedes «обидві кнопки = еспресо з преінфузією»):
- **`B1` / `COLD_FLUSH`**: `P11` → помпа `COLD_FLUSH_MS=4000` @ `COLD_FLUSH_PCT=100`, **ТЕН OFF** → `P10` (або достроковий `B1` → `P10`). Індикація через наявні `P11`/`P10` (панель без змін).
- Блок старту cold flush: активні `P21` / `HS1` / `SS1` / `LT1` (під час `HS0`/`SS0` `B1` веде в `LT1`, не в flush).
- **`B2` / `BREW_2`**: єдине еспресо — `PREINFUSION_MS=2000` @ 40 % → пауза 2000 → pot power до повторного `B2`. Зупинка «чужою» кнопкою заборонена; `B1` під час `P21` ігнорується.

**Rationale**: FR-004…007, SC-002/003; промив групи без нагріву; `B1` більше не дублює `B2`.

**Alternatives considered**: Автостоп за об’ємом — out of scope; окремий код панелі для flush — відхилено (панель зараз не міняємо); cold flush без `P11` — відхилено (користувач обрав A).

## 6. Пар: ручка, імпульси, вихід у `LT1`

**Decision** (session 2–4 + **2026-11-15 dual targets**):
- Готовність пару: **`Tt ≥ STEAM_READY_TARGET_C` (120 °C)**. Без `READY_HOLD_MS`, без смуги 118…122.
- Після `HS0` з **закритою** ручкою тримати **120 °C**, **доки** `B1`/`B2` → `LT1`.
- Падіння `Tt < 120` у **`STEAM_READY` (ручка закрита)** → авто `HS1`→`HS0`; якщо ручка вже відкрита на `HS0` — одразу `SS1` з ціллю **128 °C**.
- У **`STEAM_FLOW`**: `activeTargetC = STEAM_FLOW_TARGET_C` (**128 °C**); падіння Tt → лишитись у flow (імпульси без паузи, без `HS1`/`SS0`); ТЕН — steam-flow до 128.
- Подача: `SS1` / імпульси; закриття → `SS0`, помпа 0, ціль знову **120** → `STEAM_READY`.
- **`STEAM_IDLE_EXIT_MS=0`**; **`B3` у `HS*`/`SS*` ignore**; **`B1`/`B2` → `LT1`**; `B3`@`STEAM_COOL` → `enterSteam()`.
- **`STEAM_COOL`**: ТЕН OFF; відкрита ручка → імпульси; ready → помпа 0 → `LT0`.
- OLED пару/`STEAM_COOL`: `T_current` / `Heat_power` / `Valve open`|`Valve close` (FR-029; shared labels).
- `FAULT` → ТЕН OFF, помпа 0; вихід v1 лише power cycle.

**Rationale**: FR-009…015, FR-021/022/024–029, FR-032, SC-005…011/013/015.

**Alternatives considered**: Одна ціль на весь пар — відхилено 2026-11-15; `B3`-cancel / idle→`LT1` — відхилено session 4; пауза помпи при холодному Tt у flow — відхилено session 3.

## 6b. ПІ-профілі: heat-up + brew/steam flow

**Decision** (session 3–4 + **2026-11-15**):
- **Чотири** tunable профілі: heat-up / base / brew-flow / steam-flow (+ FF під час flow).
- Cold flush і `STEAM_COOL` — ТЕН OFF.
- Цілі без offset; `HE0`/`HS0` без змін; overshoot ~+3…5 °C OK (нижче cutoff).
- **Source of truth числових Kp/Ki/FF** = поточна секція Tunables у `scripts/013_main_brew_logic.cpp` (стенд). У ітерації 2026-11-15 **не відкочувати** до застарілих табличних 4×/2.5×.

**Rationale**: FR-027 / SC-010/012; користувач: «коефіцієнти не виправляй, спробуємо на них».

**Alternatives considered**: Повернути документи до 4×/2.5× як обов’язкові дефолти — **відхилено** (стенд важливіший).

## 7. Декальцинація (EEPROM, uptime)

**Decision (final, accepted)**: Без RTC. У EEPROM зберігати `accumulatedOnSeconds` (оновлювати кожні `DECALC_SAVE_EVERY_S`) і `dcLatched` (+ magic). Інтервал = `DECALC_INTERVAL_DAYS × 86400` с **накопиченого uptime** (час роботи під живленням). Після інтервалу — один `DC1`. Скидання: USB `DC0` → кадр `DC0` на панель + обнулення акумулятора / нова мітка. Стенд: зменшити `DECALC_INTERVAL_S` або force `DC1`.

**Rationale**: Підтверджено користувачем 2026-11-11: uptime без RTC — нормально. FR-016/023, SC-008 (допуск ≈ період save). Переживає вимкнення: акумулятор лишається в EEPROM (не тікає «календарний» час, поки машина вимкнена — це очікувано).

**Alternatives considered**:
- Wall-clock календар / RTC / Serial `TIME` — відхилено для v1 (зайве залізо; out of scope).
- Лічильник порцій — відхилено.

## 8. Дисплей (OLED layouts) і флоуметр

**Decision** (оновлено 2026-11-15, HE1/HE0 + shared labels):
- **`ESPRESSO_HEAT` / `ESPRESSO_READY` (`HE1`/`HE0`)**: FR-034 — `T_current`, `T_target`, `Heat_power`, `Pump_current` (команда), `Pump_target` (pot). Layout 128×32: **4 рядки**, помпи на одному. Refresh @ `BREW_DISPLAY_INTERVAL_MS`.
- **`BREW_2`**: FR-008 — ті самі labels для спільних полів (`T_current`/`T_target`/`Heat_power`/`Pump_current`); **без** `Pump_target`. Лічильник води на OLED **не** обов’язковий.
- **`COLD_FLUSH`**: зворотний відлік «Cold flush N s» (`N = ceil(remainingMs/1000)`); UA лише якщо є кириличний шрифт (FR-033).
- **Пар + `STEAM_COOL`**: FR-029 — `T_current` / `Heat_power` / `Valve open`|`Valve close` (short: `T_now` / `Heat` / `V open`|`V close`).
- **Спільний словник**: повний `T_current`/`T_target`/`Heat_power`/`Pump_current`/`Pump_target` (+ short `T_now`/`T_tar`/`Heat`/`P_now`/`P_tar`). Заборонені сирі `Tt`/`S`/`H`/`Pp`/`Ps` і голі `open`/`close`.
- Flowmeter імпульси можуть лишатись у внутрішній логіці/USB-лозі; `FLOW_PULSES_PER_ML` — опційно для майбутнього.

**Rationale**: Clarify 2026-11-15 (два раунди): heat/ready telemetry + readable labels на brew/steam; SSD1306 ASCII → EN cold flush і ASCII labels; 5 полів на 4 рядках через об’єднання помп.

**Alternatives considered**:
- Idle як `006` (Tt/H%/set без pump pot) — **superseded** FR-034.
- `Pump_target` також у `BREW_2` — відхилено (Option B).
- 5 окремих рядків / alternating screens — відхилено (обрано 4 рядки, помпи разом).
- Залишити water counter у brew-layout — відхилено.
- Той самий brew-layout для cold flush — відхилено (countdown).
- Залишити `Tt`/`S`/`H`/`Pp` / голі `open`/`close` — відхилено (нечитабельність).

## 9. Іменовані константи (вимога замовника)

**Decision**: Усі значущі цифрові значення параметрів, лімітів, градусів, таймінгів, потужностей і інтервалів — лише через іменовані `const` / `constexpr` / `#define` на початку файлу (секція «Tunables»). У тілі FSM/loop — нуль «магічних» літералів для бізнес-параметрів. Каталог: [contracts/tunable-constants.md](./contracts/tunable-constants.md).

**Rationale**: Швидке редагування без пошуку по коду; узгоджено з тюнінгом ПІ в `006`.

**Alternatives considered**: `Config.h` окремим файлом — опційно пізніше; v1 — одна секція в скетчі.

## 10. Тестування

**Decision**: Ручні сценарії quickstart на стенді двох Nano + USB Serial; для DC — тимчасово зменшити `DECALC_INTERVAL_*` або сервісні команди `DC1`/`DC0`.

**Rationale**: Немає unit-test інфраструктури в репо для AVR.

**Alternatives considered**: native host mocks — out of scope цієї фічі.

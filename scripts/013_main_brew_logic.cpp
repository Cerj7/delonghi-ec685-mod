// Main Brew Logic — 013_main_brew_logic.cpp
//
// База: scripts/006_thermoblock_pi_heater.cpp (+ NeoSWSerial, EEPROM).
// Цільова прошивка основної Nano: еспресо / пролив / пар / декальцинація
// (specs/001-main-brew-logic/). Heater/sensor/OLED/WDT/unused pins — з 006
// через Tunables (SAFETY_CUTOFF_C > STEAM_FLOW_TARGET_C). Panel UART + USB DC0/DC1 service;
// MachineState + shared helpers (T009: targets, ready hold, pump authority, fault;
// T011: coolOrReady).
//
// Мета (006, до портування FSM) — історичний базис; OLED labels нижче
// superseded Phase 14 (FR-008/029/034). Актуальний словник:
//   contracts/tunable-constants.md § OLED labels + contracts/main-fsm.md
//   OLED authority — short: T_now / T_tar / Heat / P_now / P_tar /
//   V open|V close (заборонено сирі Tt/S/H/Pp/Ps і голі open/close).
//   1. Потенціометр температури (A1) задає цільову температуру термоблоку
//      (TARGET_MIN_C…TARGET_MAX_C).
//   2. ПІ-регулятор за термопарою термоблоку (MAX6675 #1, CS = A2) керує
//      SSR ТЕНу (D4, HIGH = нагрів) часовим ШІМ з вікном 1 с.
//   3. OLED (013): heat/ready FR-034 @ BREW_DISPLAY_INTERVAL_MS — 3 rows:
//      T_now / (T_tar + Heat) / (P_now + P_tar); BREW_2 FR-008 — без P_tar;
//      steam/cool FR-029 — T_now / Heat / V open|V close; cold flush —
//      "Cold flush N s"; FAULT — fault.
//   4. Потенціометр помпи (A0) → диммер; уставка також на OLED як P_tar
//      лише в ESPRESSO_HEAT / ESPRESSO_READY (FR-034).
//   5. Кнопки / панель UART — FSM (див. MachineState).
//   6. Watchdog 2 с: зависання → reset → ТЕН LOW, помпа 0.
//   7. CS другої термопари (бачок, D11) не читається, але тримається HIGH —
//      інакше конфлікт на спільній шині SO і Tt читається як 0.0.
//   8. Захист: невалідне/залипле показання термопари або перегрів → ТЕН OFF.
//
// Піни — за pinout у readme.md (після перенесення термопар):
//   D2  Zero-Cross диммера (фіксовано бібліотекою AC_Dimmer, INT0)
//   D3  GATE диммера → симистор помпи
//   D4  SSR ТЕНу — вихід часового ШІМ (HIGH = нагрів)
//   D5  флоуметр — PCINT pulse counter (WaterCounter)
//   D6  штатний SDD — не використовується
//   D8  PANEL_TX → D5 плати кнопок (UART 19200)
//   D9  PANEL_RX ← D6 плати кнопок (UART 19200)
//   D10 ручка пари — isSteamHandleOpen()
//   A2  CS термопари #1 — термоблок   ← регулятор і показ
//   D11 CS термопари #2 — бачок води  ← не читаємо, але тримаємо HIGH!
//   D12 SO / D13 SCK — спільна шина MAX6675
//   A0  потенціометр помпи (PUMP_MIN_PCT…PUMP_MAX_PCT)
//   A1  потенціометр температури термоблоку (TARGET_MIN_C…TARGET_MAX_C)
//   A4 / A5 — OLED I2C
//
// Бібліотеки (Менеджер бібліотек Arduino IDE):
//   1. Adafruit GFX Library
//   2. Adafruit SSD1306
//   3. MAX6675 library (by Adafruit)
//   4. AC_dimmer-dmr-0.2.1 (zip з https://github.com/serhiy23412/AC_dimmer-dmr-0.2.1)
//   5. NeoSWSerial (панель UART @ 19200). Обов'язково в NeoSWSerial.h увімкнути
//      NEOSWSERIAL_EXTERNAL_PCINT — інакше бібліотека займає всі PCINT* і
//      конфліктує з ISR флоуметра на D5 (__vector_5). Скетч сам: PCINT0→panel,
//      PCINT2→flowmeter. (SoftwareSerial має ту саму проблему.)
//   6. EEPROM (декальцинація)
//
// Спека / план: specs/001-main-brew-logic/; docs/main-brew-logic.md (пізніше)

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <max6675.h>
#include <AC_Dimmer.h>
#include <avr/wdt.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <NeoSWSerial.h>
#include <EEPROM.h>

// =============================================================================
// Tunables — specs/001-main-brew-logic/contracts/tunable-constants.md
//             + specs/002-descaling-program/contracts/descale-tunables.md
// Усі бізнес °C / мс / % / періоди тут. У тілі FSM/loop — лише імена констант
// (допустимі 0/1 для булевої логіки, індекси циклів, розміри буферів протоколу).
// =============================================================================

// --- Temperatures (°C) ---
// Потенціометр A1: проти годинникової → TARGET_MIN_C (холодний стан / нагрів OFF),
// за годинниковою → TARGET_MAX_C. FR-001.
const float TARGET_MIN_C = 85.0f;       // Низький край потенціометра еспресо (FR-030)
const float TARGET_MAX_C = 105.0f;      // Верхній край потенціометра еспресо (FR-030)
const float TARGET_OFF_BAND_C = 1.0f;   // Біля мінімуму = нагрів OFF (не тримати 20 °C)
// Dual steam targets (FR-032): ready/HS0 = 120; flow/handle-open = 128.
const float STEAM_READY_TARGET_C = 120.0f;  // Нагрів / HS0 / auto-HS1 (ручка закрита)
const float STEAM_FLOW_TARGET_C = 132.0f;   // Підтримка під час STEAM_FLOW (ручка відкрита)
// Legacy alias = ready target; не використовувати як ціль у STEAM_FLOW (T080).
const float STEAM_TARGET_C = STEAM_READY_TARGET_C;
// FR-011: HS0 = Tt ≥ STEAM_READY_TARGET_C (без верхньої смуги). STEAM_READY_BAND_C не
// використовується для HS0; залишено для сумісності (див. contracts/tunable-constants.md).
const float STEAM_READY_BAND_C = 0.0f;
// HE0: нижня межа (Tt ≥ target − band); верхньої немає. LT0: верхня (Tt ≤ target + band).
const float READY_BAND_C = 1.0f;        // Допуск готовності еспресо / після LT1 (FR-002)
const float HEATER_DEADBAND_C = 0.5f;   // Мертва зона ПІ (інтеграл «замерзає»)
// FR-027: документований допустимий короткочасний overshoot над ціллю під час/
// після heat-up (стенд; не окремий cutoff). Орієнтир ≤ цього; FAULT лише через
// SAFETY_CUTOFF_C. Див. contracts/tunable-constants.md.
const float HEAT_UP_OVERSHOOT_OK_C = 5.0f;
// Аварійний поріг перегріву термоблоку (FR-013 / FR-018 / T032): якщо Tt ≥ цього
// значення → ТЕН OFF, помпа 0, OLED «FAULT», скидання лише power cycle.
// MUST бути строго вище STEAM_FLOW_TARGET_C (FR-032 / contracts/tunable-constants.md).
const float SAFETY_CUTOFF_C = 145.0f;
const float TEMP_VALID_MIN_C = 1.0f;    // Нижня правдоподібність термопари
const float TEMP_VALID_MAX_C = 200.0f;  // Верхня правдоподібність термопари

// --- Timing (ms unless noted) ---
const unsigned long READY_HOLD_MS = 1000;            // Утримання в смузі перед HE0/LT0 (FR-002, SC-001)
const unsigned long COLD_FLUSH_MS = 3000;            // Тривалість холодного проливу B1 (FR-004)
const unsigned long PREINFUSION_MS = 2000;           // Тривалість преінфузії (FR-004, SC-002)
const unsigned long PREINFUSION_PAUSE_MS = 2000;     // Пауза після преінфузії (FR-004, SC-002)
const unsigned long STEAM_PULSE_ON_MS = 100;         // Імпульс помпи пару ON (SC-006)
const unsigned long STEAM_PULSE_OFF_MS = 880;       // Імпульс помпи пару OFF (SC-006)
// FR-014 / SC-007: Idle→LT1 вимкнено (session 4). 0 = вимкнено. Залишено як
// Tunable на випадок стенду — contracts/tunable-constants.md.
const unsigned long STEAM_IDLE_EXIT_MS = 0;
const unsigned long HEATER_WINDOW_MS = 1000;         // Вікно часового ШІМ ТЕНу
const unsigned long CONTROL_INTERVAL_MS = 100;       // Період перерахунку ПІ
const unsigned long BLOCK_TEMP_INTERVAL_MS = 250;    // Опитування MAX6675 (≥220 мс)
const unsigned long MAX6675_BOOT_DELAY_MS = 500;     // Перша конверсія MAX6675 після живлення (setup)
const unsigned long MS_PER_S = 1000UL;               // SI: мс → с (ПІ dt, decalc uptime)
const unsigned long POT_INTERVAL_MS = 20;            // Опитування потенціометрів
const unsigned long OLED_INTERVAL_MS = 200;          // Оновлення OLED (поза brew ≥2 Гц)
const unsigned long BREW_DISPLAY_INTERVAL_MS = 500;  // OLED heat/ready/brew/flush (SC-004/016/017; FR-008/033/034)
const unsigned long SERIAL_INTERVAL_MS = 500;        // Періодичний USB-лог
const unsigned long WATCHDOG_TIMEOUT_MS = 2000;      // WDT (апаратно: WDTO_2S)
const unsigned long BUTTON_REACT_MAX_MS = 300;       // Цільовий SLA зупинки (SC-003/SC-006; тест)
const unsigned long TIMING_TOLERANCE_MS = 200;       // Допуск стендових перевірок (SC-002/SC-007)

// --- Powers (%) ---
const uint8_t COLD_FLUSH_PCT = 100;   // Холодний пролив B1 (FR-004); ТЕН OFF (FR-028)
const uint8_t PREINFUSION_PCT = 80;   // Преінфузія (FR-004, SC-002)
const uint8_t STEAM_PULSE_PCT = 100;  // Імпульс пари (SC-006)
const uint8_t PUMP_OFF_PCT = 0;       // Явний «стоп» помпи
// Потенціометр A0: проти годинникової → PUMP_MIN_PCT, за годинниковою → PUMP_MAX_PCT (FR-031).
const uint8_t PUMP_MIN_PCT = 50;      // Низький край потенціометра помпи
const uint8_t PUMP_MAX_PCT = 100;     // Верхній край потенціометра помпи

// --- PI / pots / dimmer ---
// Kp: %/°C; Ki: %/(°C·с); Ti = Kp/Ki ≈ 100 с. Калібрувати docs/thermoblock-pi-heater.md.
// Чотири профілі (FR-027 / SC-010): heat-up · base · brew-flow (~5× stand) · steam-flow (~4× stand)
// + FF під час flow. Без target-offset. Heat-up у ESPRESSO_HEAT/STEAM_HEAT; brew-flow
// лише FLOW у BREW_2; steam-flow — увесь STEAM_FLOW; cold flush / STEAM_COOL — ТЕН OFF
// (FR-028). Option C стартувало 4×/2.5×+FF; T041 stand-tune → поточні дефолти нижче.
const float HEATER_KP = 5.0f;         // Базовий ПІ Kp
const float HEATER_KI = 0.05f;        // Базовий ПІ Ki
const float HEATER_KP_HEAT = 12.0f;   // Heat-up Kp (стартово ~2.4× base; стенд) (FR-027)
const float HEATER_KI_HEAT = 0.12f;   // Heat-up Ki (стартово ~2.4× base; стенд) (FR-027)
const float HEATER_KP_BREW = 25.0f;   // Brew-flow Kp (~5× base, стенд) — лише BREW_2 && FLOW (FR-027)
const float HEATER_KI_BREW = 0.25f;   // Brew-flow Ki (~5× base, стенд) — лише BREW_2 && FLOW (FR-027)
const uint8_t HEATER_FF_BREW_PCT = 25;  // Адитивний FF % — лише BREW_2 && FLOW (FR-027)
const float HEATER_KP_STEAM = 15.0f;  // Steam-flow Kp (~3× base, стенд) — увесь STEAM_FLOW (FR-027)
const float HEATER_KI_STEAM = 0.15f; // Steam-flow Ki (~3× base, стенд) (FR-027)
const uint8_t HEATER_FF_STEAM_PCT = 15; // Адитивний FF % під час STEAM_FLOW (FR-027)
const uint8_t POT_SAMPLES = 8;                 // Усереднення АЦП
const float POT_SMOOTHING = 0.3f;              // EMA потенціометрів (0…1)
const uint8_t DIMMER_HYSTERESIS = 2;           // Гістерезис рівня диммера
const uint8_t TEMP_INVALID_STREAK_LIMIT = 3;   // Підряд невалідних читань → fault

// --- Decalc (accumulated uptime, no RTC; FR-016, SC-008) ---
const unsigned long DECALC_INTERVAL_DAYS = 28;
const unsigned long DECALC_INTERVAL_S =
    DECALC_INTERVAL_DAYS * 86400UL;  // Секунди uptime; для стенду можна зменшити
const uint16_t DECALC_EEPROM_MAGIC = 0xEC85u;  // Валідація блоку EEPROM (EC685)
const unsigned long DECALC_SAVE_EVERY_S = 60;  // Flush uptime в EEPROM (допуск SC-008)

// --- Descale program (specs/002-descaling-program/contracts/descale-tunables.md) ---
// FR-016 / SC-007: усі бізнес-літерали програми декальцинації лише тут; у тілі
// FSM/loop — імена констант. Об’ємні сценарії потребують FLOW_PULSES_PER_ML > 0.
// Hold / enter
const unsigned long DESCALE_HOLD_PROMPT_MS = 1000;     // OLED-відлік після утримання (FR-003)
const unsigned long DESCALE_HOLD_ENTER_MS = 10000;     // Утримання → DESCALE_PREP (FR-001, SC-001)
const unsigned long DESCALE_HOLD_ENTER_TOL_MS = 200;   // Допуск стенду SC-001 (тест)
// Volume / flow
const unsigned long DESCALE_PHASE_VOLUME_ML = 1000;    // Ціль об’єму фази 1 і 2 (FR-009)
// Phase 1 (clean)
const unsigned long DESCALE_PHASE1_ON_MS = 500;        // Імпульс помпи ON (FR-007, SC-003)
const unsigned long DESCALE_PHASE1_OFF_MS = 1000;      // Пауза відмочування (FR-007)
const unsigned long DESCALE_PHASE1_NOFLOW_MS = 1000;   // No-flow під час ON → refill/complete (FR-010)
// Phase 2 (rinse)
const unsigned long DESCALE_PHASE2_ON_MS = 500;        // Імпульс помпи ON (FR-008, SC-003)
const unsigned long DESCALE_PHASE2_OFF_MS = 500;       // Пауза між імпульсами (FR-008)
const unsigned long DESCALE_PHASE2_NOFLOW_MS = 1500;   // No-flow під час ON → refill/complete (FR-012)
// Heater / power
const float DESCALE_PHASE1_TEMP_C = 53.0f;             // Уставка ТЕНа лише в DESCALE_PHASE1 (FR-006)
const uint8_t DESCALE_PUMP_PCT = 100;                  // Потужність під час ON (не потенціометр)
// Timing tolerances (test aids)
const unsigned long DESCALE_PULSE_TOL_MS = 50;         // Допуск SC-003
const unsigned long DESCALE_DC0_MAX_LATENCY_MS = 1000; // Допуск SC-005 (DC0 після фази 2)

// --- Link / buffers (contracts/panel-uart.md; як у 012_main_panel_link.cpp) ---
// D9 RX ← D6 панелі, D8 TX → D5 панелі, 8N1 @ LINK_BAUD; USB debug @ PC_BAUD.
const uint8_t PANEL_RX_PIN = 9;
const uint8_t PANEL_TX_PIN = 8;
const long LINK_BAUD = 19200;
const long PC_BAUD = 115200;
const uint8_t LINK_CODE_MAX_LEN = 3;  // Макс. довжина коду панелі (напр. P11)

// --- Flow display / volume ---
// 0 = показувати імпульси флоуметра; >0 = мл (імпульси / FLOW_PULSES_PER_ML).
// Descale volume (FR-009 / SC volume scenarios): MUST be > 0 у проді / стенді
// з об’ємом; інакше мл з імпульсів недоступні (див. descale-tunables.md).
const float FLOW_PULSES_PER_ML = 0.0f;

// --- OLED 0.91" 128x32 I2C (SDA=A4, SCL=A5) ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define OLED_I2C_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Термопара термоблоку (MAX6675 #1, CS = A2) ---
#define THERMO_SO 12
#define THERMO_SCK 13
#define THERMO_CS_BLOCK A2
// CS другого модуля (бачок). Не читаємо, але тримати HIGH ОБОВ'ЯЗКОВО —
// див. setupUnusedPins().
#define THERMO_CS_TANK 11
MAX6675 thermoT(THERMO_SCK, THERMO_CS_BLOCK, THERMO_SO);

// --- Піни ---
#define DIMMER_GATE_PIN 3
#define HEATER_PIN 4
#define FLOWMETER_PIN 5
#define SDD_PIN 6
#define STEAM_HANDLE_PIN 10

#define PUMP_POT_PIN A0
#define TEMP_POT_PIN A1
#define PUMP_DIMMER 0  // номер каналу диммера в AC_Dimmer (від 0)

NeoSWSerial panel(PANEL_RX_PIN, PANEL_TX_PIN);

// --- Panel UART framing (contracts/panel-uart.md; як у 012_main_panel_link.cpp) ---
char linkBuf[LINK_CODE_MAX_LEN + 1];
uint8_t linkLen = 0;

// --- USB Serial service (contracts/panel-uart.md Service Serial; FR-023 DC0) ---
char serviceBuf[LINK_CODE_MAX_LEN + 1];
uint8_t serviceLen = 0;

// --- Потужність помпи: калібрувальна крива диммера (літерали в масиві OK) ---
// Частка потужності (%) → рівень диммера AC_Dimmer (0…240).
// Диммер ріже фазу: рівень — затримка відкриття симистора після zero-cross
// (у бібліотеці: t = (19850 − 77·level) · 0.5 мкс). Див. docs/dimmer.md.
struct DimmerPoint {
  uint8_t percent;
  uint8_t level;
};

const DimmerPoint DIMMER_CURVE[] = {
    {0, 0},   {5, 51},   {10, 65},  {15, 76},  {20, 85},  {25, 94},  {30, 101},
    {35, 108}, {40, 115}, {45, 121}, {50, 128}, {55, 134}, {60, 141}, {65, 148},
    {70, 155}, {75, 162}, {80, 170}, {85, 180}, {90, 191}, {95, 205}, {100, 240},
};
const uint8_t DIMMER_CURVE_POINTS = sizeof(DIMMER_CURVE) / sizeof(DIMMER_CURVE[0]);

// --- Стан: термопара ---
float tempBlockC = NAN;
bool tempValid = false;
uint8_t tempInvalidStreak = 0;
uint8_t zeroTempStreak = 0;

// --- Стан: помпа ---
// potPumpPercent — A0 → PUMP_MIN_PCT…PUMP_MAX_PCT (для FLOW); pumpCommandPct — фактична команда.
uint8_t potPumpPercent = PUMP_MIN_PCT;
uint8_t pumpCommandPct = PUMP_OFF_PCT;
uint8_t pumpDimmerLevel = 0;  // фактично застосований рівень диммера
float pumpPercentFiltered = 0.0f;  // згладжена позиція ручки A0, 0…100

// --- Стан: уставки (data-model TemperatureSetpoint) ---
enum TargetMode : uint8_t {
  TARGET_ESPRESSO = 0,
  TARGET_STEAM,
};

TargetMode targetMode = TARGET_ESPRESSO;
float espressoTargetC = TARGET_MIN_C;  // з A1 (TARGET_MIN_C…TARGET_MAX_C)
float activeTargetC = TARGET_MIN_C;    // espresso / STEAM_READY_TARGET_C / STEAM_FLOW_TARGET_C
float targetPercentFiltered = 0.0f;    // згладжена позиція ручки A1, 0…100

// --- Ready criterion (READY_BAND_C + READY_HOLD_MS) ---
bool readyBandLatched = false;
unsigned long readyBandSinceMs = 0;

// --- Стан: ТЕН ---
uint8_t heaterPercent = 0;    // вихід ПІ = середня потужність за вікно, %
float heaterIntegral = 0.0f;  // інтегральна складова, %
bool heaterGateOn = false;    // фактичний стан виходу D4
unsigned long heaterWindowStartMs = 0;

bool displayOk = false;

unsigned long lastBlockTempMs = 0;
unsigned long lastPotMs = 0;
unsigned long lastControlMs = 0;
unsigned long lastOledMs = 0;
unsigned long lastSerialMs = 0;

// --- FSM (data-model.md / contracts/main-fsm.md) — без COLD_START ---
// Power-on → ESPRESSO_HEAT (HE1). Handlers filled in user stories.
enum MachineState : uint8_t {
  ESPRESSO_HEAT = 0,
  ESPRESSO_READY,
  COLD_FLUSH,
  BREW_2,
  STEAM_HEAT,
  STEAM_READY,
  STEAM_FLOW,
  STEAM_COOL,
  // Descale FSM (data-model.md / contracts/descale-fsm.md) — stubs T006; handlers US1+
  DESCALE_HOLD,
  DESCALE_PREP,
  DESCALE_PHASE1,
  DESCALE_REFILL,
  DESCALE_WAIT_RINSE,
  DESCALE_PHASE2,
  DESCALE_REFILL_WATER,
  FAULT,
};

MachineState machineState = ESPRESSO_HEAT;

// --- DescaleHoldGesture (data-model.md; RAM only) ---
// Arms R3 routing (panel-hold-uart.md). Hold enter/cancel rules → T012+.
// Defaults: inactive → R3 no-op until hold arms active.
struct DescaleHoldGesture {
  unsigned long pressMs;  // millis() on B3
  bool active;            // waiting for R3 / timeout enter
  bool countdownVisible;  // after DESCALE_HOLD_PROMPT_MS
};

DescaleHoldGesture descaleHoldGesture = {0UL, false, false};

// Resume ESPRESSO_READY / ESPRESSO_HEAT after hold cancel (R3 mid-countdown).
// Captured in armDescaleHold(); not part of DescaleHoldGesture data-model row.
MachineState descaleHoldResumeState = ESPRESSO_READY;

// Last OLED countdown N while DESCALE_HOLD (force refresh on second change).
unsigned int lastDescaleCountdownSec = 0xFFFF;

// T014 / T021 / T022 / T025 / FR-005: OLED HANDLE_CLOSED overlay after B3 with
// closed steam handle in DESCALE_PREP / REFILL / WAIT_RINSE / REFILL_WATER.
bool descaleHandleClosedPrompt = false;

// --- DescaleSession (data-model.md; RAM only — no EEPROM phase checkpoint) ---
// Power cycle does not resume; phase / pulses live only in RAM.
enum DescaleSessionPhase : uint8_t {
  DESCALE_SESSION_NONE = 0,
  DESCALE_SESSION_PHASE1,
  DESCALE_SESSION_PHASE2,
};

enum DescalePulsePhase : uint8_t {
  DESCALE_PULSE_PHASE_ON = 0,
  DESCALE_PULSE_PHASE_OFF,
};

struct DescaleSession {
  DescaleSessionPhase phase;
  uint32_t phasePulses;               // pulses since phase start (refill keeps)
  DescalePulsePhase pulsePhase;       // ON | OFF
  unsigned long pulsePhaseStartedMs;  // start of current ON/OFF slice
  unsigned long lastFlowPulseMs;      // updated from ISR/counter during phase
  bool noFlowArmed;                   // true only during ON
};

DescaleSession descaleSession = {
    DESCALE_SESSION_NONE, 0UL, DESCALE_PULSE_PHASE_ON, 0UL, 0UL, false,
};

// --- DescaleUiPrompt (data-model.md; FR-017) ---
// IDs + Flash (F()/PROGMEM) UA strings. Show helpers below in OLED section.
// 128×32 @ text size 1 ≈ 21×4 glyphs; shorten/wrap only on overflow.
enum DescaleUiPrompt : uint8_t {
  DESCALE_UI_COUNTDOWN = 0,
  DESCALE_UI_PREP,
  DESCALE_UI_HANDLE_CLOSED,
  DESCALE_UI_PHASE1_RUN,
  DESCALE_UI_REFILL,
  DESCALE_UI_WAIT_RINSE,
  DESCALE_UI_PHASE2_RUN,
  DESCALE_UI_REFILL_WATER,
};

// --- Brew profile phase (data-model BrewProfile; FR-004) ---
enum BrewPhase : uint8_t {
  BREW_PHASE_PREINFUSE = 0,
  BREW_PHASE_PAUSE,
  BREW_PHASE_FLOW,
};

BrewPhase brewPhase = BREW_PHASE_PREINFUSE;
unsigned long brewPhaseStartMs = 0;

// SteamPulse (data-model): ON/OFF while STEAM_FLOW, handle open.
enum SteamPulsePhase : uint8_t {
  STEAM_PULSE_PHASE_ON = 0,
  STEAM_PULSE_PHASE_OFF,
};

SteamPulsePhase steamPulsePhase = STEAM_PULSE_PHASE_ON;
unsigned long steamPulsePhaseStartMs = 0;

// --- Inputs: steam handle + flowmeter (data-model SteamPulse / WaterCounter) ---
// INPUT_PULLUP + оптопара на GND → активний сигнал = LOW (як у 002/003).
// Якщо OLED показує OPEN при закритій ручці (або навпаки) — інвертувати цей прапор.
const bool STEAM_HANDLE_ACTIVE_LOW = false;

// WaterCounter.pulses — імпульси за поточний пролив; reset на старті COLD_FLUSH / BREW_2.
volatile uint32_t flowPulses = 0;

// --- DecalcTimer (data-model.md / research §7; FR-016, FR-023, SC-008) ---
// EEPROM layout @ DECALC_EEPROM_ADDR: magic | accumulatedOnSeconds | dcLatched.
// Non-blocking reminder only: dcLatched MUST NEVER gate brew/heat/steam FSM.
// EEPROM.flush deferred during COLD_FLUSH/BREW_2/STEAM_FLOW so AVR EEPROM write (~ms/byte)
// cannot stall pump/heater timing (SC-008 / constitution §III).
const int DECALC_EEPROM_ADDR = 0;
const unsigned long DECALC_MS_PER_S = MS_PER_S;  // ms → whole uptime seconds

struct DecalcEepromLayout {
  uint16_t magic;
  uint32_t accumulatedOnSeconds;
  uint8_t dcLatched;  // 0/1 у EEPROM
};

uint32_t accumulatedOnSeconds = 0;
bool dcLatched = false;
bool decalcEepromDirty = false;           // RAM ahead of EEPROM; flush when safe
unsigned long lastDecalcMs = 0;           // last millis sample for uptime
unsigned long decalcMsCarry = 0;          // leftover ms toward next whole second
unsigned long secondsSinceDecalcSave = 0; // toward DECALC_SAVE_EVERY_S flush

// ---------------------------------------------------------------------------
// Steam handle (D10) + flowmeter (D5)
// ---------------------------------------------------------------------------
// Ручка пари відкрита → true (подача пари / STEAM_FLOW).
bool isSteamHandleOpen() {
  const int raw = digitalRead(STEAM_HANDLE_PIN);
  return STEAM_HANDLE_ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
}

// PCINT ownership (NEOSWSERIAL_EXTERNAL_PCINT у NeoSWSerial.h):
//   PCINT0 / Port B — panel RX D9 → NeoSWSerial::rxISR(PINB)
//   PCINT2 / Port D — flowmeter D5 (PCINT21)
// На Nano зовнішні INT лише D2/D3; D2 зайнятий Zero-Cross диммера.
ISR(PCINT0_vect) {
  NeoSWSerial::rxISR(PINB);
}

ISR(PCINT2_vect) {
  static uint8_t lastState = HIGH;
  const uint8_t state = digitalRead(FLOWMETER_PIN);
  // Зростаючий фронт (як у 001/002/003).
  if (lastState == LOW && state == HIGH) {
    flowPulses++;
  }
  lastState = state;
}

void setupFlowmeterInterrupt() {
  pinMode(FLOWMETER_PIN, INPUT_PULLUP);
  PCICR |= (1 << PCIE2);
  PCMSK2 |= (1 << PCINT21);
}

// WaterCounter.reset — викликати на вході в COLD_FLUSH / BREW_2.
void resetFlowPulses() {
  noInterrupts();
  flowPulses = 0;
  interrupts();
}

uint32_t getFlowPulses() {
  noInterrupts();
  const uint32_t pulses = flowPulses;
  interrupts();
  return pulses;
}

// Опційний poll-шлях (без подвійного підрахунку з ISR): лише зчитування лічильника.
uint32_t pollFlowPulses() {
  return getFlowPulses();
}

// WaterCounter.displayValue: pulses, або мл якщо FLOW_PULSES_PER_ML > 0.
float flowDisplayValue(uint32_t pulses) {
  if (FLOW_PULSES_PER_ML > 0.0f) {
    return (float)pulses / FLOW_PULSES_PER_ML;
  }
  return (float)pulses;
}

// DescaleSession.phaseVolumeMl (derived): phasePulses / FLOW_PULSES_PER_ML.
// FR-009 / research §5: volume criterion requires FLOW_PULSES_PER_ML > 0.
float descalePhaseVolumeMl() {
  if (FLOW_PULSES_PER_ML <= 0.0f) {
    return 0.0f;
  }
  return (float)descaleSession.phasePulses / FLOW_PULSES_PER_ML;
}

// T018 / FR-009: phase volume ≥ DESCALE_PHASE_VOLUME_ML (compare hook for T019/T020).
bool descalePhaseVolumeReached() {
  if (FLOW_PULSES_PER_ML <= 0.0f) {
    return false;
  }
  return descalePhaseVolumeMl() >= (float)DESCALE_PHASE_VOLUME_ML;
}

// T018 / FR-009: fold ISR flow counter into DescaleSession.phasePulses;
// stamp lastFlowPulseMs on new pulses (no-flow arm uses it in T019).
void accumulateDescalePhasePulses() {
  const uint32_t delta = getFlowPulses();
  if (delta == 0UL) {
    return;
  }
  descaleSession.phasePulses += delta;
  descaleSession.lastFlowPulseMs = millis();
  resetFlowPulses();
}

// ---------------------------------------------------------------------------
// Помпа (диммер)
// ---------------------------------------------------------------------------
// Частка потужності (%) → рівень диммера за оберненою RMS-кривою.
uint8_t powerPercentToDimmerLevel(uint8_t percent) {
  if (percent >= 100) {
    return DIMMER_CURVE[DIMMER_CURVE_POINTS - 1].level;
  }

  for (uint8_t i = 1; i < DIMMER_CURVE_POINTS; i++) {
    if (percent > DIMMER_CURVE[i].percent) {
      continue;
    }
    const DimmerPoint &lo = DIMMER_CURVE[i - 1];
    const DimmerPoint &hi = DIMMER_CURVE[i];
    const uint16_t spanPercent = hi.percent - lo.percent;
    const uint16_t spanLevel = hi.level - lo.level;
    return lo.level + ((uint16_t)(percent - lo.percent) * spanLevel) / spanPercent;
  }
  return 0;
}

// AC_Dimmer пише в спільний масив, який читає ISR zero-cross. Виклик робимо
// атомарно, щоб ISR не побачив напівоновлене значення.
void setPumpLevel(uint8_t level) {
  noInterrupts();
  Dimm_value(PUMP_DIMMER, level);
  interrupts();
  pumpDimmerLevel = level;
}

void applyPumpPercent(uint8_t percent) {
  const uint8_t level = powerPercentToDimmerLevel(percent);
  const int16_t delta = (int16_t)level - (int16_t)pumpDimmerLevel;
  if (delta >= DIMMER_HYSTERESIS || delta <= -DIMMER_HYSTERESIS || percent == PUMP_OFF_PCT) {
    // OFF always forces level 0 (bypass hysteresis so stop is immediate).
    if (percent == PUMP_OFF_PCT) {
      setPumpLevel(0);
    } else {
      setPumpLevel(level);
    }
  }
}

// Команда помпи (profile / pulse / pot у FLOW). Поза brew/steam flow — enforcePumpAuthority().
void setPumpPercent(uint8_t percent) {
  if (percent > 100) {
    percent = 100;
  }
  pumpCommandPct = percent;
  applyPumpPercent(percent);
}

// Pump authority (contracts/main-fsm.md): non-zero лише в COLD_FLUSH / BREW_2 /
// STEAM_FLOW / STEAM_COOL (pulse while handle open — FR-028 / T073).
// Descale (contracts/descale-fsm.md / FR-007/008): PHASE1/PHASE2 лише під час
// pulse ON — prep/wait/refill/HOLD і pulse OFF → PUMP_OFF_PCT.
// Усі інші стани → PUMP_OFF_PCT незалежно від pot A0.
bool pumpMayRun() {
  if (machineState == COLD_FLUSH || machineState == BREW_2 ||
      machineState == STEAM_FLOW || machineState == STEAM_COOL) {
    return true;
  }
  if ((machineState == DESCALE_PHASE1 || machineState == DESCALE_PHASE2) &&
      descaleSession.pulsePhase == DESCALE_PULSE_PHASE_ON) {
    return true;
  }
  return false;
}

void enforcePumpAuthority() {
  if (!pumpMayRun() && pumpCommandPct != PUMP_OFF_PCT) {
    setPumpPercent(PUMP_OFF_PCT);
  } else if (!pumpMayRun() && pumpDimmerLevel != 0) {
    setPumpPercent(PUMP_OFF_PCT);
  }
}

// ---------------------------------------------------------------------------
// Descale heater gates (FR-006 / contracts/descale-fsm.md)
// ---------------------------------------------------------------------------
// True for any DESCALE_* program/hold state (FR-014 / T010 fault abort scope).
bool isDescaleMachineState() {
  return machineState == DESCALE_HOLD || machineState == DESCALE_PREP ||
         machineState == DESCALE_PHASE1 || machineState == DESCALE_REFILL ||
         machineState == DESCALE_WAIT_RINSE || machineState == DESCALE_PHASE2 ||
         machineState == DESCALE_REFILL_WATER;
}

// DESCALE_HOLD: не викликати force-off — штатний еспресо-ПІ (countdown).
// DESCALE_PHASE1: єдиний стан з уставкою DESCALE_PHASE1_TEMP_C (не espresso/steam).
// PREP / REFILL / WAIT_RINSE / PHASE2 / REFILL_WATER: ТЕН 0 %, без ПІ.
bool descaleHeaterForceOff() {
  return machineState == DESCALE_PREP || machineState == DESCALE_REFILL ||
         machineState == DESCALE_WAIT_RINSE || machineState == DESCALE_PHASE2 ||
         machineState == DESCALE_REFILL_WATER;
}

// true → PHASE1 owns PI; *outSetpointC = DESCALE_PHASE1_TEMP_C.
// false → не PHASE1 (HOLD = espresso path; force-off states → descaleHeaterForceOff).
bool descaleHeaterSetpoint(float *outSetpointC) {
  if (machineState != DESCALE_PHASE1) {
    return false;
  }
  if (outSetpointC != nullptr) {
    *outSetpointC = DESCALE_PHASE1_TEMP_C;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Уставки / ready / fault (data-model.md, contracts/main-fsm.md)
// ---------------------------------------------------------------------------
void refreshActiveTarget();  // used by setTargetMode (defined below)

// Параметр — uint8_t, не TargetMode: Arduino IDE генерує прототипи до enum
// у .ino, інакше «TargetMode was not declared in this scope».
void setTargetMode(uint8_t mode) {
  targetMode = static_cast<TargetMode>(mode);
  // refreshActiveTarget gates DESCALE_* (FR-006): PHASE1 → 53 °C; force-off
  // states skip espresso/steam; HOLD / non-descale → espresso or steam as before.
  refreshActiveTarget();
}

void refreshActiveTarget() {
  // FR-006: DESCALE_* never takes espresso/steam setpoint except HOLD countdown.
  float descaleSp = 0.0f;
  if (descaleHeaterSetpoint(&descaleSp)) {
    activeTargetC = descaleSp;
    return;
  }
  if (descaleHeaterForceOff()) {
    return;
  }
  if (targetMode == TARGET_STEAM) {
    activeTargetC = (machineState == STEAM_FLOW) ? STEAM_FLOW_TARGET_C
                                                 : STEAM_READY_TARGET_C;
  } else {
    activeTargetC = espressoTargetC;
  }
}

void resetReadyHold() {
  readyBandLatched = false;
  readyBandSinceMs = 0;
}

// HE0 (ESPRESSO_HEAT): Tt ≥ espressoTargetC − READY_BAND_C протягом READY_HOLD_MS.
// Верхньої межі немає — heat-up overshoot не блокує готовність (FR-002 / SC-001).
bool espressoReadyCriterion(unsigned long now) {
  if (!tempValid || isnan(tempBlockC)) {
    resetReadyHold();
    return false;
  }

  if (tempBlockC >= espressoTargetC - READY_BAND_C) {
    if (!readyBandLatched) {
      readyBandLatched = true;
      readyBandSinceMs = now;
    }
    return (now - readyBandSinceMs) >= READY_HOLD_MS;
  }

  resetReadyHold();
  return false;
}

// LT0 (STEAM_COOL): Tt ≤ espressoTargetC + READY_BAND_C протягом READY_HOLD_MS.
// Одностороння верхня межа — дзеркало HE0: «достатньо охололо» після пару (FR-028).
// Не можна ділити критерій HE0: Tt ≫ target було б «готово» одразу після LT1.
bool espressoCoolReadyCriterion(unsigned long now) {
  if (!tempValid || isnan(tempBlockC)) {
    resetReadyHold();
    return false;
  }

  if (tempBlockC <= espressoTargetC + READY_BAND_C) {
    if (!readyBandLatched) {
      readyBandLatched = true;
      readyBandSinceMs = now;
    }
    return (now - readyBandSinceMs) >= READY_HOLD_MS;
  }

  resetReadyHold();
  return false;
}

// Steam HS0: Tt ≥ STEAM_READY_TARGET_C — миттєво, без hold і без верхньої смуги
// (FR-011, SC-005, T080).
bool steamReadyCriterion() {
  if (!tempValid || isnan(tempBlockC)) {
    return false;
  }
  return tempBlockC >= STEAM_READY_TARGET_C;
}

void showFaultOled() {
  if (!displayOk) {
    return;
  }
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(2);
  display.setCursor(0, 8);
  display.print(F("FAULT"));
  display.display();
  lastOledMs = millis();
}

void sendToPanel(const char *code);  // defined with panel UART section
void handleServiceDc0();               // USB + descale phase-2 success (FR-013)

// Latched FAULT: heater OFF, pump 0, OLED fault. Exit v1 = power cycle only.
// T022 / quickstart S6 / SC fault stop: abort active flush/brew/steam panel
// frames so LEDs are not left dangling (P11→P10, P21→P20, SS1→SS0).
// FR-014 / T010 / contracts/descale-fsm.md: from any DESCALE_* — same latch
// (ТЕН OFF, PUMP_OFF_PCT); MUST NOT send DC0 / clear DecalcTimer (dcLatched /
// accumulatedOnSeconds / EEPROM untouched — never call handleServiceDc0 here).
void enterFault() {
  if (machineState == FAULT) {
    heaterPercent = 0;
    heaterIntegral = 0.0f;
    digitalWrite(HEATER_PIN, LOW);
    heaterGateOn = false;
    setPumpPercent(PUMP_OFF_PCT);
    return;
  }

  // Abort active portion / steam flow / descale program before latching.
  if (machineState == COLD_FLUSH) {
    sendToPanel("P10");
  } else if (machineState == BREW_2) {
    sendToPanel("P20");
  } else if (machineState == STEAM_FLOW) {
    sendToPanel("SS0");
  } else if (isDescaleMachineState()) {
    // Drop hold + session; leave DC1 / DecalcTimer as-is (no DC0).
    descaleHoldGesture = {0UL, false, false};
    descaleSession = {
        DESCALE_SESSION_NONE, 0UL, DESCALE_PULSE_PHASE_ON, 0UL, 0UL, false,
    };
  }

  machineState = FAULT;
  heaterPercent = 0;
  heaterIntegral = 0.0f;
  digitalWrite(HEATER_PIN, LOW);
  heaterGateOn = false;
  setPumpPercent(PUMP_OFF_PCT);
  resetReadyHold();
  showFaultOled();
  Serial.println(F("# FAULT latched — power cycle to clear"));
}

// ---------------------------------------------------------------------------
// Потенціометри
// ---------------------------------------------------------------------------
uint16_t readPotAverage(uint8_t pin) {
  uint16_t sum = 0;
  for (uint8_t i = 0; i < POT_SAMPLES; i++) {
    sum += analogRead(pin);
  }
  return sum / POT_SAMPLES;
}

void updatePots(unsigned long now) {
  if (now - lastPotMs < POT_INTERVAL_MS) {
    return;
  }
  lastPotMs = now;

  // --- Помпа A0 → potPumpPercent (PUMP_MIN_PCT…PUMP_MAX_PCT); диммер — setPumpPercent / authority ---
  const float pumpPercentRaw = (float)readPotAverage(PUMP_POT_PIN) * 100.0f / 1023.0f;

  pumpPercentFiltered += (pumpPercentRaw - pumpPercentFiltered) * POT_SMOOTHING;
  if (pumpPercentFiltered < 0.0f) {
    pumpPercentFiltered = 0.0f;
  }
  if (pumpPercentFiltered > 100.0f) {
    pumpPercentFiltered = 100.0f;
  }
  potPumpPercent = (uint8_t)(PUMP_MIN_PCT +
                              (PUMP_MAX_PCT - PUMP_MIN_PCT) * pumpPercentFiltered / 100.0f +
                              0.5f);

  // --- Температура: A1 → espressoTargetC (TARGET_MIN_C…TARGET_MAX_C) ---
  const float targetPercentRaw =
      (float)readPotAverage(TEMP_POT_PIN) * 100.0f / 1023.0f;

  targetPercentFiltered +=
      (targetPercentRaw - targetPercentFiltered) * POT_SMOOTHING;
  if (targetPercentFiltered < 0.0f) {
    targetPercentFiltered = 0.0f;
  }
  if (targetPercentFiltered > 100.0f) {
    targetPercentFiltered = 100.0f;
  }
  espressoTargetC =
      TARGET_MIN_C + (TARGET_MAX_C - TARGET_MIN_C) * targetPercentFiltered / 100.0f;
  refreshActiveTarget();
}

// ---------------------------------------------------------------------------
// ТЕН: часове ШІМ
// ---------------------------------------------------------------------------
// heaterPercent — середня потужність за вікно HEATER_WINDOW_MS: спочатку вікна
// нагрів увімкнений на percent·вікно, решта вікна — пауза.
//
// Викликається КОЖЕН loop, а не раз на CONTROL_INTERVAL_MS: інакше момент
// вимкнення квантувався б до CONTROL_INTERVAL_MS, і на сталій потужності 6 %
// реально було б грубіше (квант CONTROL_INTERVAL_MS із HEATER_WINDOW_MS) —
// регулятор не міг би тримати ціль точно.
void updateHeaterGate(unsigned long now) {
  if (heaterPercent == 0) {
    digitalWrite(HEATER_PIN, LOW);
    heaterGateOn = false;
    return;
  }

  if (now - heaterWindowStartMs >= HEATER_WINDOW_MS) {
    heaterWindowStartMs = now;
  }
  const uint16_t onTimeMs =
      (uint16_t)((uint32_t)heaterPercent * HEATER_WINDOW_MS / 100UL);
  const bool on = (now - heaterWindowStartMs) < onTimeMs;

  digitalWrite(HEATER_PIN, on ? HIGH : LOW);
  heaterGateOn = on;
}

// ---------------------------------------------------------------------------
// ТЕН: ПІ-регулятор за термопарою термоблоку
// ---------------------------------------------------------------------------
// FR-013 / T032 / T080: у TARGET_STEAM ПІ тримає activeTargetC =
// STEAM_READY_TARGET_C (120) або STEAM_FLOW_TARGET_C (128) — не pot A1 —
// до виходу з пару (B1/B2 → coolOrReady; idle→LT1 вимкнено — T071). FR-018:
// невалідна Tt або Tt ≥ SAFETY_CUTOFF_C → ТЕН OFF (+ FAULT на cutoff / streak).
void updateHeaterControl(unsigned long now) {
  if (now - lastControlMs < CONTROL_INTERVAL_MS) {
    return;
  }
  const unsigned long dtMs = now - lastControlMs;
  lastControlMs = now;

  // Нагрів вимкнено: FAULT, COLD_FLUSH (FR-004), STEAM_COOL (FR-028 / T073),
  // descale force-off (FR-006: PREP/REFILL/WAIT_RINSE/PHASE2/REFILL_WATER),
  // ручка в мінімумі (лише espresso), термопара, cutoff. У всіх цих випадках
  // інтеграл скидається, щоб після повернення в робочий режим не було
  // «пострілу» накопиченою потужністю. (Heat-up PI select = T074.)
  // DESCALE_HOLD не тут — лишає еспресо-ПІ (FR-006 countdown).
  // FR-014 / T010: force-off DESCALE_* still honors SAFETY_CUTOFF → enterFault
  // (no DC0); HOLD/PHASE1 use the shared cutoff path below.
  if (machineState == FAULT || machineState == COLD_FLUSH ||
      machineState == STEAM_COOL || descaleHeaterForceOff()) {
    heaterIntegral = 0.0f;
    heaterPercent = 0;
    if (descaleHeaterForceOff() && tempValid &&
        tempBlockC >= SAFETY_CUTOFF_C) {
      enterFault();
    }
    return;
  }

  // FR-006: лише DESCALE_PHASE1 — уставка DESCALE_PHASE1_TEMP_C (не pot/steam).
  float descaleSp = 0.0f;
  const bool descalePhase1Pi = descaleHeaterSetpoint(&descaleSp);
  if (descalePhase1Pi) {
    activeTargetC = descaleSp;
  }

  // TARGET_OFF_BAND стосується лише еспресо: у парі pot A1 не гасить ТЕН (FR-013).
  // DESCALE_PHASE1 ігнорує pot off-band — інакше мін. A1 гасив би 53 °C ПІ.
  const bool espressoOffBand =
      !descalePhase1Pi && targetMode == TARGET_ESPRESSO &&
      espressoTargetC <= TARGET_MIN_C + TARGET_OFF_BAND_C;
  const bool heaterAllowed = tempValid && !espressoOffBand;
  // FR-018 / T032: SAFETY_CUTOFF_C захисний і під час утримання пару (120/128).
  if (!heaterAllowed || tempBlockC >= SAFETY_CUTOFF_C) {
    heaterIntegral = 0.0f;
    heaterPercent = 0;
    if (tempValid && tempBlockC >= SAFETY_CUTOFF_C) {
      enterFault();
    }
    return;
  }

  // FR-027 / SC-010/012 / T074: профіль кожен CONTROL_INTERVAL_MS (≤ 1 тік).
  // heat-up — ESPRESSO_HEAT / STEAM_HEAT лише поки Tt < activeTargetC;
  // brew-flow лише BREW_2 && FLOW; steam-flow — STEAM_FLOW; інакше base
  // (ready / hold-in-band / preinfuse / pause / STEAM_READY / DESCALE_PHASE1).
  // FF адитивний лише під час flow; без target-offset; SAFETY_CUTOFF_C спільний (вище).
  // HE0/HS0 критерії не змінюються тут.
  float kp = HEATER_KP;
  float ki = HEATER_KI;
  float ffPct = 0.0f;
  if ((machineState == ESPRESSO_HEAT || machineState == STEAM_HEAT) &&
      tempBlockC < activeTargetC) {
    kp = HEATER_KP_HEAT;
    ki = HEATER_KI_HEAT;
  } else if (machineState == BREW_2 && brewPhase == BREW_PHASE_FLOW) {
    kp = HEATER_KP_BREW;
    ki = HEATER_KI_BREW;
    ffPct = (float)HEATER_FF_BREW_PCT;
  } else if (machineState == STEAM_FLOW) {
    kp = HEATER_KP_STEAM;
    ki = HEATER_KI_STEAM;
    ffPct = (float)HEATER_FF_STEAM_PCT;
  }

  const float error = activeTargetC - tempBlockC;
  const float dt = (float)dtMs / (float)MS_PER_S;
  const float p = kp * error;

  // Інтегратор: у мертвій зоні «замерзає» — шум термопари не накопичується.
  if (fabs(error) >= HEATER_DEADBAND_C) {
    float iNext = heaterIntegral + ki * error * dt;
    if (iNext < 0.0f) {
      iNext = 0.0f;
    }
    if (iNext > 100.0f) {
      iNext = 100.0f;
    }

    // Anti-windup: якщо вихід у насиченні й похибка «розганяє» його далі —
    // інтеграл не накопичуємо (інакше після виходу з насичення буде перегрів).
    const float raw = p + iNext + ffPct;
    if ((raw > 100.0f && error > 0.0f) || (raw < 0.0f && error < 0.0f)) {
      iNext = heaterIntegral;
    }
    heaterIntegral = iNext;
  }

  float output = p + heaterIntegral + ffPct;
  if (output < 0.0f) {
    output = 0.0f;
  }
  if (output > 100.0f) {
    output = 100.0f;
  }

  heaterPercent = (uint8_t)(output + 0.5f);
}

// ---------------------------------------------------------------------------
// Термопара
// ---------------------------------------------------------------------------
void updateTemperature(unsigned long now) {
  if (now - lastBlockTempMs < BLOCK_TEMP_INTERVAL_MS) {
    return;
  }
  lastBlockTempMs = now;

  tempBlockC = thermoT.readCelsius();

  // Діагностика: справжня термопара в кімнаті дає ~20–25 °C, тому рівно 0.0
  // (а не NAN, як при обриві) означає проблему на шині, а не холод.
  if (!isnan(tempBlockC) && tempBlockC < 0.1f && zeroTempStreak < 255) {
    zeroTempStreak++;
    if (zeroTempStreak == 4) {
      Serial.println(F("# WARN Tt=0.0 — перевір CS термопари бачка (D11) = HIGH "
                       "і CS термоблоку (A2)"));
    }
  } else if (tempBlockC >= 0.1f) {
    zeroTempStreak = 0;
  }

  // Валідність для регулятора: не NAN, у правдоподібних межах і не «залиплий»
  // нуль. Інакше ПІ бачив би «дуже холодно» і тримав ТЕН на 100 % наосліп.
  // FR-014 / T010: invalid TC from any DESCALE_* → enterFault (no DC0) same as
  // brew/steam; enterFault clears hold/session without DecalcTimer clear.
  const bool plausible = !isnan(tempBlockC) && tempBlockC > TEMP_VALID_MIN_C &&
                         tempBlockC < TEMP_VALID_MAX_C && zeroTempStreak == 0;
  if (plausible) {
    tempInvalidStreak = 0;
    tempValid = true;
    return;
  }

  if (tempInvalidStreak < 255) {
    tempInvalidStreak++;
  }
  if (tempInvalidStreak >= TEMP_INVALID_STREAK_LIMIT) {
    const bool wasValid = tempValid;
    tempValid = false;
    if (wasValid || machineState != FAULT) {
      enterFault();
    }
  }
}

// ---------------------------------------------------------------------------
// OLED
// ---------------------------------------------------------------------------
// T084 / FR-033 / SC-016: COLD_FLUSH OLED — countdown only (not brew-metrics).
// N = ceil(remainingMs/1000); EN ASCII for default SSD1306 font.
void showColdFlushOled(unsigned long now) {
  if (!displayOk) {
    return;
  }

  const unsigned long elapsed = now - brewPhaseStartMs;
  const unsigned long remainingMs =
      (elapsed >= COLD_FLUSH_MS) ? 0UL : (COLD_FLUSH_MS - elapsed);
  // Integer ceil for non-negative ms: ceil(x/1000) == (x + 999) / 1000.
  const unsigned int nSec = (unsigned int)((remainingMs + 999UL) / 1000UL);

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 12);
  display.print(F("Cold flush "));
  display.print(nSec);
  display.print(F(" s"));
  display.display();
}

// T089 / FR-034 / SC-017: ESPRESSO_HEAT / ESPRESSO_READY OLED — 5 values on 3 rows
// (T_tar+Heat combined; P_now / P_tar spaced). Y = 0 / 12 / 24 for even gaps on 128×32.
// Labels from contracts/tunable-constants.md § OLED labels (no raw Tt/S/H/Pp/Ps).
// Pump row uses short aliases (width); routed from updateOled @ BREW_DISPLAY_INTERVAL_MS (T090).
void showEspressoHeatReadyOled() {
  if (!displayOk) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Row 1 — T_current (tempBlockC); short T_now fits 128 px with decimals.
  display.setCursor(0, 0);
  display.print(F("T_now="));
  if (isnan(tempBlockC)) {
    display.print(F("---"));
  } else {
    display.print(tempBlockC, 1);
    display.print(F("C"));
  }

  // Row 2 — T_target + Heat_power on one line (activeTargetC == espressoTargetC).
  // Heat at x=72 — same column as P_tar; gap stays wide for 2–3 digit T_tar.
  display.setCursor(0, 12);
  display.print(F("T_tar="));
  display.print((int)(activeTargetC + 0.5f));
  display.setCursor(72, 12);
  display.print(F("Heat="));
  display.print(heaterPercent);
  display.print(F("%"));

  // Row 3 — Pump_current (pumpCommandPct) + Pump_target (potPumpPercent / A0).
  display.setCursor(0, 24);
  display.print(F("P_now="));
  display.print(pumpCommandPct);
  display.setCursor(72, 24);
  display.print(F("P_tar="));
  display.print(potPumpPercent);

  display.display();
}

// T083 / T091 / FR-008 / SC-004: BREW_2 OLED — T_current, T_target, Heat_power,
// Pump_current on 3 rows (T_tar+Heat combined) @ BREW_DISPLAY_INTERVAL_MS
// (no Pump_target; water counter N/A). Same Y = 0 / 12 / 24 as heat/ready.
void showBrew2Oled() {
  if (!displayOk) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Row 1 — T_current (tempBlockC); short T_now.
  display.setCursor(0, 0);
  display.print(F("T_now="));
  if (isnan(tempBlockC)) {
    display.print(F("---"));
  } else {
    display.print(tempBlockC, 1);
    display.print(F("C"));
  }

  // Row 2 — T_target + Heat_power on one line (same x=72 column as heat/ready).
  display.setCursor(0, 12);
  display.print(F("T_tar="));
  display.print((int)(activeTargetC + 0.5f));
  display.setCursor(72, 12);
  display.print(F("Heat="));
  display.print(heaterPercent);
  display.print(F("%"));

  // Row 3 — Pump_current (pumpCommandPct); short P_now. No Pump_target (FR-008).
  display.setCursor(0, 24);
  display.print(F("P_now="));
  display.print(pumpCommandPct);
  display.print(F("%"));

  display.display();
}

// T085 / T092 / FR-029 / SC-013: steam + STEAM_COOL OLED —
// T_current, Heat_power, Valve open|close (short T_now / Heat / V open|V close).
// У STEAM_COOL Heat_power MUST = 0 (FR-029 Option A).
void showSteamOled() {
  if (!displayOk) {
    return;
  }

  const uint8_t heaterPctShown =
      (machineState == STEAM_COOL) ? 0 : heaterPercent;

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Row 1 — T_current (tempBlockC); short T_now.
  display.setCursor(0, 0);
  display.print(F("T_now="));
  if (isnan(tempBlockC)) {
    display.print(F("---"));
  } else {
    display.print(tempBlockC, 1);
    display.print(F("C"));
  }

  // Row 2 — Heat_power (heaterPercent); short Heat; cool always 0.
  display.setCursor(0, 12);
  display.print(F("Heat="));
  display.print(heaterPctShown);
  display.print(F("%"));

  // Row 3 — Valve open|close (FR-024 / FR-029); short V open|V close.
  display.setCursor(0, 24);
  if (isSteamHandleOpen()) {
    display.print(F("V open"));
  } else {
    display.print(F("V close"));
  }

  display.display();
}

// T009 / FR-017: DescaleUiPrompt → UA Flash lines (data-model.md).
// Y = 0/8/16/24 (4 rows @ size 1 on 128×32). FSM refresh rules → T012+.
// countdownSec: remaining whole seconds for COUNTDOWN only.
void showDescaleUiPrompt(DescaleUiPrompt id, unsigned int countdownSec) {
  if (!displayOk) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  switch (id) {
    case DESCALE_UI_COUNTDOWN:
      // «Режим декальцинації через N сек»
      display.setCursor(0, 0);
      display.print(F("Режим декальцинації"));
      display.setCursor(0, 8);
      display.print(F("через "));
      display.print(countdownSec);
      display.print(F(" сек"));
      break;

    case DESCALE_UI_PREP:
      // «Залийте 1 л очисника, відкрийте ручку та натисніть кнопку "Пар"»
      display.setCursor(0, 0);
      display.print(F("Залийте 1 л очисника,"));
      display.setCursor(0, 8);
      display.print(F("відкрийте ручку та"));
      display.setCursor(0, 16);
      display.print(F("натисніть кнопку Пар"));
      break;

    case DESCALE_UI_HANDLE_CLOSED:
      // Пояснення: відкрийте ручку пари
      display.setCursor(0, 0);
      display.print(F("Відкрийте ручку пари"));
      break;

    case DESCALE_UI_PHASE1_RUN:
      // «Виконується декальцинація: фаза очистки»
      display.setCursor(0, 0);
      display.print(F("Виконується"));
      display.setCursor(0, 8);
      display.print(F("декальцинація:"));
      display.setCursor(0, 16);
      display.print(F("фаза очистки"));
      break;

    case DESCALE_UI_REFILL:
      // «Долийте очисник і натисніть кнопку "Пар"»
      display.setCursor(0, 0);
      display.print(F("Долийте очисник і"));
      display.setCursor(0, 8);
      display.print(F("натисніть кнопку Пар"));
      break;

    case DESCALE_UI_WAIT_RINSE:
      // «Залийте 1 л води та натисніть кнопку "Пар"»
      display.setCursor(0, 0);
      display.print(F("Залийте 1 л води та"));
      display.setCursor(0, 8);
      display.print(F("натисніть кнопку Пар"));
      break;

    case DESCALE_UI_PHASE2_RUN:
      // Виконання промивання (дзеркало PHASE1_RUN)
      display.setCursor(0, 0);
      display.print(F("Виконується"));
      display.setCursor(0, 8);
      display.print(F("декальцинація:"));
      display.setCursor(0, 16);
      display.print(F("фаза промивання"));
      break;

    case DESCALE_UI_REFILL_WATER:
      // «Долийте воду і натисніть кнопку "Пар"»
      display.setCursor(0, 0);
      display.print(F("Долийте воду і"));
      display.setCursor(0, 8);
      display.print(F("натисніть кнопку Пар"));
      break;

    default:
      break;
  }

  display.display();
}

// T012 / FR-003: whole seconds remaining until DESCALE_HOLD_ENTER_MS (ceil).
unsigned int descaleHoldCountdownSec(unsigned long now) {
  if (!descaleHoldGesture.active) {
    return 0;
  }
  const unsigned long elapsed = now - descaleHoldGesture.pressMs;
  if (elapsed >= DESCALE_HOLD_ENTER_MS) {
    return 0;
  }
  const unsigned long remainingMs = DESCALE_HOLD_ENTER_MS - elapsed;
  return (unsigned int)((remainingMs + 999UL) / 1000UL);
}

// Map DESCALE_* → prompt id. HOLD countdown N is computed by callers (T012).
// Returns false if state has no static prompt (e.g. HOLD before countdown).
bool showDescaleOledForState(unsigned int countdownSec) {
  switch (machineState) {
    case DESCALE_HOLD:
      if (!descaleHoldGesture.countdownVisible) {
        return false;
      }
      showDescaleUiPrompt(DESCALE_UI_COUNTDOWN, countdownSec);
      return true;
    case DESCALE_PREP:
      // T014: HANDLE_CLOSED overlay until open-handle B3 starts PHASE1.
      if (descaleHandleClosedPrompt) {
        showDescaleUiPrompt(DESCALE_UI_HANDLE_CLOSED, 0);
      } else {
        showDescaleUiPrompt(DESCALE_UI_PREP, 0);
      }
      return true;
    case DESCALE_PHASE1:
      showDescaleUiPrompt(DESCALE_UI_PHASE1_RUN, 0);
      return true;
    case DESCALE_REFILL:
      // T021: HANDLE_CLOSED overlay until open-handle B3 resumes PHASE1.
      if (descaleHandleClosedPrompt) {
        showDescaleUiPrompt(DESCALE_UI_HANDLE_CLOSED, 0);
      } else {
        showDescaleUiPrompt(DESCALE_UI_REFILL, 0);
      }
      return true;
    case DESCALE_WAIT_RINSE:
      // T022: HANDLE_CLOSED overlay until open-handle B3 starts PHASE2.
      if (descaleHandleClosedPrompt) {
        showDescaleUiPrompt(DESCALE_UI_HANDLE_CLOSED, 0);
      } else {
        showDescaleUiPrompt(DESCALE_UI_WAIT_RINSE, 0);
      }
      return true;
    case DESCALE_PHASE2:
      showDescaleUiPrompt(DESCALE_UI_PHASE2_RUN, 0);
      return true;
    case DESCALE_REFILL_WATER:
      // T025: HANDLE_CLOSED overlay until open-handle B3 resumes PHASE2.
      if (descaleHandleClosedPrompt) {
        showDescaleUiPrompt(DESCALE_UI_HANDLE_CLOSED, 0);
      } else {
        showDescaleUiPrompt(DESCALE_UI_REFILL_WATER, 0);
      }
      return true;
    default:
      return false;
  }
}

void updateOled(unsigned long now) {
  // T083 / T084 / T090: heat/ready + BREW_2 + COLD_FLUSH @ BREW_DISPLAY_INTERVAL_MS.
  const bool brewDisplay =
      (machineState == ESPRESSO_HEAT || machineState == ESPRESSO_READY ||
       machineState == COLD_FLUSH || machineState == BREW_2);
  const unsigned long interval =
      brewDisplay ? BREW_DISPLAY_INTERVAL_MS : OLED_INTERVAL_MS;
  if (!displayOk || now - lastOledMs < interval) {
    return;
  }
  lastOledMs = now;

  if (machineState == FAULT) {
    showFaultOled();
    return;
  }

  // T090 / FR-034 / SC-017: do not use legacy 006-style idle layout for these.
  if (machineState == ESPRESSO_HEAT || machineState == ESPRESSO_READY) {
    showEspressoHeatReadyOled();
    return;
  }

  if (machineState == BREW_2) {
    showBrew2Oled();
    return;
  }

  if (machineState == COLD_FLUSH) {
    showColdFlushOled(now);
    return;
  }

  // T085 / FR-029: STEAM_HEAT / STEAM_READY / STEAM_FLOW / STEAM_COOL.
  if (machineState == STEAM_HEAT || machineState == STEAM_READY ||
      machineState == STEAM_FLOW || machineState == STEAM_COOL) {
    showSteamOled();
    return;
  }

  // T012 / FR-003: DESCALE_HOLD — espresso metrics until prompt, then countdown N.
  if (machineState == DESCALE_HOLD) {
    if (descaleHoldGesture.countdownVisible) {
      showDescaleUiPrompt(DESCALE_UI_COUNTDOWN, descaleHoldCountdownSec(now));
    } else {
      showEspressoHeatReadyOled();
    }
    return;
  }

  // T009 / T014: other DESCALE_* static prompts (+ HANDLE_CLOSED in PREP)
  if (showDescaleOledForState(0)) {
    return;
  }

  // T095 fallback OLED (non-brew/steam/fault): shared short labels only
  // (contracts/tunable-constants.md § OLED labels — no Tt/S/H/Pp/Ps).
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  // Рядок 1 — T_current (tempBlockC); short T_now.
  // «!» у кінці = показання невалідне (обрив, конфлікт на шині) → ТЕН OFF.
  display.setCursor(0, 0);
  display.print(F("T_now="));
  if (isnan(tempBlockC)) {
    display.print(F("---"));
  } else {
    display.print(tempBlockC, 1);
    display.print(F("C"));
  }
  if (!tempValid) {
    display.print(F("!"));
  }

  // Рядок 2 — Heat_power (heaterPercent); short Heat.
  display.setCursor(0, 12);
  display.print(F("Heat="));
  display.print(heaterPercent);
  display.print(F("%"));

  // Рядок 3 — T_target (activeTargetC); short T_tar (fallback / non-espresso).
  display.setCursor(0, 24);
  display.print(F("T_tar="));
  display.print((int)(activeTargetC + 0.5f));

  display.display();
}

// ---------------------------------------------------------------------------
// Serial
// ---------------------------------------------------------------------------
void logSerial(unsigned long now) {
  if (now - lastSerialMs < SERIAL_INTERVAL_MS) {
    return;
  }
  lastSerialMs = now;

  // t_ms,Tt_C,temp_valid,set_C,heater_pct,heater_on,pump_cmd_pct,dimmer_0_255,steam_h,state
  Serial.print(now);
  Serial.print(',');
  if (isnan(tempBlockC)) {
    Serial.print(F("nan"));
  } else {
    Serial.print(tempBlockC, 2);
  }
  Serial.print(',');
  Serial.print(tempValid ? 1 : 0);
  Serial.print(',');
  Serial.print(activeTargetC, 1);
  Serial.print(',');
  Serial.print(heaterPercent);
  Serial.print(',');
  Serial.print(heaterGateOn ? 1 : 0);
  Serial.print(',');
  Serial.print(pumpCommandPct);
  Serial.print(',');
  Serial.print(pumpDimmerLevel);
  Serial.print(',');
  Serial.print(isSteamHandleOpen() ? 1 : 0);
  Serial.print(',');
  Serial.println((uint8_t)machineState);
}

// ---------------------------------------------------------------------------
// Panel UART (egress + ingress framing)
// ---------------------------------------------------------------------------
void sendToPanel(const char *code) {
  panel.print(code);
  panel.write('\n');

  Serial.print(F("→ panel "));
  Serial.println(code);
}

// Steam exit (session 4 / FR-014/028 / T072): ONLY from B1/B2 in HS1/HS0/
// STEAM_FLOW (steam-hold SS0). MUST NOT call from handle-close SS0, idle,
// B3, or Tt drop in STEAM_FLOW. Body: stop pulses → espresso target → LT1/
// STEAM_COOL with heater OFF. Pulse/ready loop in STEAM_COOL = T073.
// contracts/main-fsm.md — coolOrReady()
void coolOrReady() {
  // FR-018: FAULT latched until power cycle — never auto-recover via coolOrReady
  if (machineState == FAULT) {
    return;
  }

  // 1. Stop steam pulse pump if active (e.g. exit from STEAM_FLOW).
  setPumpPercent(PUMP_OFF_PCT);
  steamPulsePhase = STEAM_PULSE_PHASE_ON;
  steamPulsePhaseStartMs = millis();

  // 2. Espresso pot orient; steam ready/flow (120/128) hold ends.
  setTargetMode(TARGET_ESPRESSO);

  // 3. Heater OFF + LT1 / STEAM_COOL (FR-028). Contract MAY short-cut to LT0
  // if espresso ready criterion already holds; primary UX path is always LT1.
  heaterPercent = 0;
  heaterIntegral = 0.0f;
  digitalWrite(HEATER_PIN, LOW);
  heaterGateOn = false;

  if (machineState != STEAM_COOL) {
    sendToPanel("LT1");
  }
  machineState = STEAM_COOL;
  resetReadyHold();
}

// FR-004 / T066 / T068: B1 @ ESPRESSO_READY → P11, COLD_FLUSH, counter reset,
// pump COLD_FLUSH_PCT; ТЕН forced OFF (без ПІ-підтримки уставки).
// MUST NOT start from BREW_2 / STEAM_HEAT / STEAM_READY (HS0/SS0) / STEAM_FLOW /
// STEAM_COOL — those block flush (FR-004/FR-022); HS0/SS0 B1 = steam-exit (T070).
void startColdFlush() {
  // T068: sole entry is ESPRESSO_READY; refuse any other machine state.
  if (machineState != ESPRESSO_READY) {
    return;
  }
  sendToPanel("P11");
  machineState = COLD_FLUSH;
  resetFlowPulses();
  brewPhaseStartMs = millis();  // stamp flush start (timeout → COLD_FLUSH_MS)
  setPumpPercent(COLD_FLUSH_PCT);
  heaterPercent = 0;
  heaterIntegral = 0.0f;
  digitalWrite(HEATER_PIN, LOW);
  heaterGateOn = false;
}

// FR-005 / SC-003 / T067: repeat B1 @ COLD_FLUSH → early stop
// (PUMP_OFF_PCT, P10, ESPRESSO_READY; бюджет реакції ≤ BUTTON_REACT_MAX_MS).
void stopColdFlush() {
  setPumpPercent(PUMP_OFF_PCT);
  sendToPanel("P10");
  machineState = ESPRESSO_READY;
}

// FR-006 / T023: B2 @ ESPRESSO_READY → P21, BREW_2, counter reset, same PREINFUSE.
void startBrew2() {
  sendToPanel("P21");
  machineState = BREW_2;
  resetFlowPulses();
  brewPhase = BREW_PHASE_PREINFUSE;
  brewPhaseStartMs = millis();
  setPumpPercent(PREINFUSION_PCT);
}

// FR-007 / SC-003 / T024: repeat B2 @ BREW_2 → pump off, P20, ESPRESSO_READY
// (цільовий бюджет реакції: BUTTON_REACT_MAX_MS).
void stopBrew2() {
  setPumpPercent(PUMP_OFF_PCT);
  sendToPanel("P20");
  machineState = ESPRESSO_READY;
}

// FR-009 / FR-025 / T044 / T050: enter / return to steam (ESPRESSO_READY or STEAM_COOL via B3).
// contracts/main-fsm.md — enterSteam()
void enterSteam() {
  // FR-018: FAULT latched until power cycle — never enter steam from fault.
  // T016: no dcLatched/DC1 gate — short «пар» and cool-interrupt steam alike.
  if (machineState == FAULT) {
    return;
  }

  setTargetMode(TARGET_STEAM);  // activeTargetC = STEAM_READY_TARGET_C (120)

  // FR-009 / FR-011 / SC-007 / T080: Tt ≥ STEAM_READY_TARGET_C → HS0 immediately
  // (no band/hold); if handle already open → SS1 / STEAM_FLOW @ 128.
  if (steamReadyCriterion()) {
    sendToPanel("HS0");
    machineState = STEAM_READY;
    if (isSteamHandleOpen()) {
      sendToPanel("SS1");
      machineState = STEAM_FLOW;
      activeTargetC = STEAM_FLOW_TARGET_C;
      steamPulsePhase = STEAM_PULSE_PHASE_ON;
      steamPulsePhaseStartMs = millis();
      setPumpPercent(STEAM_PULSE_PCT);
    }
    return;
  }

  sendToPanel("HS1");
  machineState = STEAM_HEAT;
}

// FR-006 / SC-002 / T018: BREW_2 espresso profile — PREINFUSE → PAUSE → FLOW.
// During PREINFUSE/PAUSE ignore pot A0; FLOW tracks live potPumpPercent.
// Guard: if brewPhaseStartMs was stamped with millis() after loop's `now`
// (B2 handled earlier in the same iteration), unsigned (now - start)
// would underflow and skip the phase in ~0 ms — hence the now < start check.
void updateBrewPhases(unsigned long now) {
  switch (brewPhase) {
    case BREW_PHASE_PREINFUSE:
      // Hold fixed PREINFUSION_PCT (pot ignored).
      setPumpPercent(PREINFUSION_PCT);
      if (now >= brewPhaseStartMs && (now - brewPhaseStartMs) >= PREINFUSION_MS) {
        brewPhase = BREW_PHASE_PAUSE;
        brewPhaseStartMs = now;
        setPumpPercent(PUMP_OFF_PCT);
      }
      break;

    case BREW_PHASE_PAUSE:
      // Hold pump off for PREINFUSION_PAUSE_MS (pot ignored).
      setPumpPercent(PUMP_OFF_PCT);
      if (now >= brewPhaseStartMs && (now - brewPhaseStartMs) >= PREINFUSION_PAUSE_MS) {
        brewPhase = BREW_PHASE_FLOW;
        brewPhaseStartMs = now;
        setPumpPercent(potPumpPercent);
      }
      break;

    case BREW_PHASE_FLOW:
      // Live pot A0 until stop button (T019/T024).
      setPumpPercent(potPumpPercent);
      break;
  }
}

// ---------------------------------------------------------------------------
// FSM dispatch stubs (contracts/main-fsm.md button matrix + loop tick)
// ---------------------------------------------------------------------------
// T011 / FR-001, FR-002, FR-018: arm hold from espresso ready/heat.
// Does NOT enterSteam(); short-press steam path is R3 in T012.
void armDescaleHold() {
  // Capture resume target before leaving READY/HEAT (cancel path, T012).
  descaleHoldResumeState = machineState;
  descaleHoldGesture.pressMs = millis();
  descaleHoldGesture.active = true;
  descaleHoldGesture.countdownVisible = false;
  lastDescaleCountdownSec = 0xFFFF;
  machineState = DESCALE_HOLD;
}

// T012: clear hold gesture + OLED countdown bookkeeping.
void clearDescaleHoldGesture() {
  descaleHoldGesture = {0UL, false, false};
  lastDescaleCountdownSec = 0xFFFF;
}

// T013 / FR-001, FR-004, FR-004a: HOLD elapsed ≥ ENTER → DESCALE_PREP.
// Independent of dcLatched/DC1. Clears hold gesture so R3 after entry
// MUST NOT cancel; sticky hold must not start pump (new B3 → T014).
void enterDescalePrep() {
  clearDescaleHoldGesture();
  descaleHandleClosedPrompt = false;
  descaleSession = {
      DESCALE_SESSION_NONE, 0UL, DESCALE_PULSE_PHASE_ON, 0UL, 0UL, false,
  };
  setPumpPercent(PUMP_OFF_PCT);
  // descale-fsm MAY: neutralize brew/steam LEDs once so DC1 stays visible.
  sendToPanel("SS0");
  sendToPanel("HS0");
  sendToPanel("LT0");
  // Heater force-off via descaleHeaterForceOff() once state is PREP (FR-004).
  machineState = DESCALE_PREP;
  lastOledMs = 0;  // force OLED PREP «Залийте 1 л очисника…»
}

// T015 / FR-006, FR-006a: PREP (or later refill resume) → DESCALE_PHASE1.
// OLED PHASE1_RUN; PI @ DESCALE_PHASE1_TEMP_C; pulse ON immediately (no temp-gate).
// Pulse ON/OFF slices advanced in updateMachineState (T017); refill resume → T021.
// T018 / FR-009: fresh entry zeros phasePulses; resume from refill keeps them;
// ISR counter reset so only new pulses accumulate on top of kept total.
void enterDescalePhase1() {
  clearDescaleHoldGesture();
  descaleHandleClosedPrompt = false;

  const bool resume = (descaleSession.phase == DESCALE_SESSION_PHASE1);
  const uint32_t keptPulses = resume ? descaleSession.phasePulses : 0UL;
  const unsigned long now = millis();

  descaleSession.phase = DESCALE_SESSION_PHASE1;
  descaleSession.phasePulses = keptPulses;
  descaleSession.pulsePhase = DESCALE_PULSE_PHASE_ON;
  descaleSession.pulsePhaseStartedMs = now;
  descaleSession.lastFlowPulseMs = now;
  descaleSession.noFlowArmed = true;
  resetFlowPulses();

  machineState = DESCALE_PHASE1;
  // FR-006: heater setpoint 53 °C via descaleHeaterSetpoint / refreshActiveTarget.
  refreshActiveTarget();
  // FR-006a: start pulse path immediately — pumpMayRun true while pulse ON.
  setPumpPercent(DESCALE_PUMP_PCT);
  lastOledMs = 0;  // force OLED PHASE1_RUN «Виконується декальцинація: фаза очистки»
}

// T019 / FR-010: no-flow during pulse ON + volume < target → DESCALE_REFILL.
// Keep phasePulses / session phase (resume → T021); keep DC1 (no DC0/clear).
// Pump 0; heater 0 % via descaleHeaterForceOff once state is REFILL.
// FR-014a: no auto-timeout abort while waiting for refill B3.
void enterDescaleRefill() {
  descaleHandleClosedPrompt = false;
  descaleSession.noFlowArmed = false;
  setPumpPercent(PUMP_OFF_PCT);
  machineState = DESCALE_REFILL;
  lastOledMs = 0;  // OLED REFILL «Долийте очисник і натисніть кнопку "Пар"»
}

// T020 / FR-011, FR-014: phase-1 volume ≥ DESCALE_PHASE_VOLUME_ML (or no-flow
// ON with volume already ≥ target) → DESCALE_WAIT_RINSE. Pump 0; heater 0 %
// via descaleHeaterForceOff. MUST NOT send DC0 / clear DecalcTimer (DC1 stays).
void enterDescaleWaitRinse() {
  descaleHandleClosedPrompt = false;
  descaleSession.noFlowArmed = false;
  setPumpPercent(PUMP_OFF_PCT);
  machineState = DESCALE_WAIT_RINSE;
  lastOledMs = 0;  // OLED WAIT_RINSE «Залийте 1 л води та натисніть кнопку "Пар"»
}

// T022 / FR-005, FR-014a: WAIT_RINSE → DESCALE_PHASE2 (fresh phase zeros
// phasePulses). Resume from REFILL_WATER keeps pulses (T025). Heater stays
// force-off. Pulse ON/OFF slices advanced in updateMachineState (T023).
// Volume / no-flow → T024 (REFILL_WATER or success stub).
void enterDescalePhase2() {
  descaleHandleClosedPrompt = false;

  const bool resume = (descaleSession.phase == DESCALE_SESSION_PHASE2);
  const uint32_t keptPulses = resume ? descaleSession.phasePulses : 0UL;
  const unsigned long now = millis();

  descaleSession.phase = DESCALE_SESSION_PHASE2;
  descaleSession.phasePulses = keptPulses;
  descaleSession.pulsePhase = DESCALE_PULSE_PHASE_ON;
  descaleSession.pulsePhaseStartedMs = now;
  descaleSession.lastFlowPulseMs = now;
  descaleSession.noFlowArmed = true;
  resetFlowPulses();

  machineState = DESCALE_PHASE2;
  // FR-006: heater force-off via descaleHeaterForceOff (PHASE2).
  // T023: entry arms pulse ON; ON/OFF slices in updateMachineState.
  setPumpPercent(DESCALE_PUMP_PCT);
  lastOledMs = 0;  // force OLED PHASE2_RUN «Виконується декальцинація: фаза промивання»
}

// T024 / FR-012: no-flow during phase-2 ON + volume < target → DESCALE_REFILL_WATER.
// Keep phasePulses / session phase (resume → T025); keep DC1 (MUST NOT DC0).
// Pump 0; heater 0 % via descaleHeaterForceOff once state is REFILL_WATER.
// FR-014a: no auto-timeout abort while waiting for refill-water B3.
void enterDescaleRefillWater() {
  descaleHandleClosedPrompt = false;
  descaleSession.noFlowArmed = false;
  setPumpPercent(PUMP_OFF_PCT);
  machineState = DESCALE_REFILL_WATER;
  lastOledMs = 0;  // OLED REFILL_WATER «Долийте воду і натисніть кнопку "Пар"»
}

// T024 / T026 / FR-012a, FR-013, SC-005: phase-2 volume ≥ DESCALE_PHASE_VOLUME_ML
// (or no-flow ON with volume already ≥ target) → success. Pump 0; shared clear
// identical to USB DC0 via handleServiceDc0() (sendToPanel("DC0") + dcLatched=false
// + accumulatedOnSeconds=0 + EEPROM) synchronously so latency ≪
// DESCALE_DC0_MAX_LATENCY_MS; then coolOrReady() → espresso heat-or-ready
// (TARGET_ESPRESSO / STEAM_COOL→ready — not steam target).
void completeDescalePhase2Success() {
  descaleHandleClosedPrompt = false;
  descaleSession.noFlowArmed = false;
  setPumpPercent(PUMP_OFF_PCT);
  descaleSession = {
      DESCALE_SESSION_NONE, 0UL, DESCALE_PULSE_PHASE_ON, 0UL, 0UL, false,
  };
  // FR-013 / SC-005: same DecalcTimer clear as USB path (sync ≪ DESCALE_DC0_MAX_LATENCY_MS).
  handleServiceDc0();
  coolOrReady();
}

// T012 / FR-003: R3 with prompt ≤ elapsed < enter — cancel countdown, no steam.
void cancelDescaleHoldToEspresso() {
  clearDescaleHoldGesture();
  setPumpPercent(PUMP_OFF_PCT);
  setTargetMode(TARGET_ESPRESSO);
  machineState = descaleHoldResumeState;
  if (machineState != ESPRESSO_READY && machineState != ESPRESSO_HEAT) {
    machineState = ESPRESSO_HEAT;
  }
  lastOledMs = 0;
}

// Central button entry: framing delivers B1/B2/B3 as '1'/'2'/'3'.
// R3 (steam release) → onPanelSteamRelease(); B3 from READY/HEAT arms
// DESCALE_HOLD (T011); short steam via R3 is T012 (panel-hold-uart.md).
void onPanelButton(char code) {
  // FR-018 / T011: FAULT — ignore all buttons (no hold, no steam)
  if (machineState == FAULT) {
    return;
  }

  // 001 matrix: ESPRESSO_HEAT ignores B1/B2; B3 may arm DESCALE_HOLD (T011)
  if (machineState == ESPRESSO_HEAT && code != '3') {
    return;
  }

  switch (machineState) {
    case ESPRESSO_READY:
      if (code == '1') {
        startColdFlush();
      } else if (code == '2') {
        startBrew2();
      } else if (code == '3') {
        // T011: B3 → DESCALE_HOLD + pressMs; not enterSteam() (R3 → T012)
        armDescaleHold();
      }
      break;

    case ESPRESSO_HEAT:
      // T011 / descale-fsm: B3 → DESCALE_HOLD (same as READY)
      if (code == '3') {
        armDescaleHold();
      }
      break;

    case COLD_FLUSH:
      // FR-005 / T067: repeat B1 → early stop; B2/B3 ignored (no side effects)
      // T011: brew — ignore B3 for DESCALE_HOLD
      if (code == '1') {
        stopColdFlush();
      }
      // code == '2' / '3': ignore
      break;

    case BREW_2:
      // FR-007 / T024 / T068: only B2 stops; B1 MUST NOT start cold flush (FR-004);
      // B1/B3 — no pump/mode/indication side effects
      // T011: brew — ignore B3 for DESCALE_HOLD
      if (code == '2') {
        stopBrew2();
      }
      break;

    case STEAM_HEAT:
      // FR-010/014/021/022 / T070: B1/B2 → coolOrReady() (ONLY steam exit to LT1);
      // B3 ignore (supersedes T028 B3-cancel). B1 MUST NOT start cold flush (FR-004).
      // T011: steam — ignore B3 for DESCALE_HOLD
      if (code == '1' || code == '2') {
        coolOrReady();
      }
      // code == '3': ignore
      break;

    case STEAM_READY:
      // FR-010/014/021/022 / T070: B1/B2 → coolOrReady(); B3 ignore (NOT cancel, NOT SS1).
      // Supersedes T029 B3-cancel. B1 @ HS0/SS0 is steam-exit, not flush (FR-004).
      // T011: steam — ignore B3 for DESCALE_HOLD
      if (code == '1' || code == '2') {
        coolOrReady();
      }
      // code == '3': ignore
      break;

    case STEAM_FLOW:
      // FR-010/014/021/022 / T070: B1/B2 → coolOrReady(); B3 ignore.
      // B1/B2 MUST NOT start cold flush (FR-004); supersedes T031/T050 B1-ignore here.
      // T011: steam — ignore B3 for DESCALE_HOLD
      if (code == '1' || code == '2') {
        coolOrReady();
      }
      // code == '3': ignore
      break;

    case STEAM_COOL:
      // FR-025 / SC-009 / T050: B3 → enterSteam() (interrupt cool; HS0 if hot else HS1)
      // T068 / FR-004/FR-022: B1/B2 block flush/brew during LT1…LT0
      // T011: steam — do not arm DESCALE_HOLD
      if (code == '3') {
        enterSteam();
      }
      break;

    case DESCALE_HOLD:
      // T029 / descale-fsm button matrix: B1/B2 ignore; B3 already held —
      // ignore (R3 → onPanelSteamRelease: short steam / cancel / race PREP).
      break;

    case DESCALE_PREP:
      // T014 / T029 / FR-004a, FR-005, FR-015: new B3 front + open handle →
      // PHASE1; closed handle + B3 → stay + OLED HANDLE_CLOSED; ignore B1/B2
      // (no cold flush / espresso). Sticky hold that entered PREP does not
      // re-fire B3 (UART edge) → no pump. R3 ignored via inactive gesture.
      if (code == '3') {
        if (isSteamHandleOpen()) {
          enterDescalePhase1();
        } else {
          descaleHandleClosedPrompt = true;
          lastOledMs = 0;
        }
      }
      // code == '1' / '2': ignore (no cold flush / brew)
      break;

    case DESCALE_REFILL:
      // T021 / T029 / FR-005, FR-014a, FR-015: new B3 + open handle → resume
      // PHASE1 (keeps phasePulses + 53 °C); closed → HANDLE_CLOSED; ignore
      // B1/B2 (no cold flush / espresso). R3 ignored (no active hold).
      // No auto-timeout abort while waiting (FR-014a).
      if (code == '3') {
        if (isSteamHandleOpen()) {
          enterDescalePhase1();
        } else {
          descaleHandleClosedPrompt = true;
          lastOledMs = 0;
        }
      }
      // code == '1' / '2': ignore
      break;

    case DESCALE_WAIT_RINSE:
      // T022 / T029 / FR-005, FR-014a, FR-015: new B3 + open handle → PHASE2
      // (zeros phasePulses); closed → HANDLE_CLOSED; ignore B1/B2 (no cold
      // flush / espresso). R3 ignored (no active hold). No auto-timeout.
      if (code == '3') {
        if (isSteamHandleOpen()) {
          enterDescalePhase2();
        } else {
          descaleHandleClosedPrompt = true;
          lastOledMs = 0;
        }
      }
      // code == '1' / '2': ignore
      break;

    case DESCALE_REFILL_WATER:
      // T025 / T029 / FR-012, FR-014a, FR-015: new B3 + open handle → resume
      // PHASE2 (keeps phasePulses; heater 0 %); closed → HANDLE_CLOSED;
      // ignore B1/B2 (no cold flush / espresso). R3 ignored (no active hold).
      // No auto-timeout abort while waiting (FR-014a).
      if (code == '3') {
        if (isSteamHandleOpen()) {
          enterDescalePhase2();
        } else {
          descaleHandleClosedPrompt = true;
          lastOledMs = 0;
        }
      }
      // code == '1' / '2': ignore
      break;

    case DESCALE_PHASE1:
    case DESCALE_PHASE2:
      // T028 / T029 / US4 / descale-fsm: B1/B2/B3 ignored (no flush/brew/
      // cancel). Mid-phase handle close is hydraulic-only — never gated here.
      // R3 no-op (gesture inactive). Pulses continue until volume/no-flow/FAULT.
      break;

    case FAULT:
      // unreachable: early return above
      break;
  }
}

// panel-hold-uart.md / descale-fsm.md / T012 + T013:
// R3 without active hold-gesture = no-op (incl. after PREP entry — FR-004a).
// elapsed < prompt → short steam (enterSteam / 001 matrix);
// prompt ≤ elapsed < enter → cancel countdown, restore READY/HEAT, no steam;
// elapsed ≥ enter → enterDescalePrep (race if tick not yet run); never cancel.
void onPanelSteamRelease() {
  if (!descaleHoldGesture.active) {
    return;
  }
  if (machineState != DESCALE_HOLD) {
    // Gesture stale outside HOLD (e.g. already PREP) — drop arm, do not steam
    // or cancel program (FR-004a).
    clearDescaleHoldGesture();
    return;
  }

  const unsigned long now = millis();
  const unsigned long elapsed = now - descaleHoldGesture.pressMs;

  if (elapsed < DESCALE_HOLD_PROMPT_MS) {
    // T016 / SC-001 / FR-001 edge: short «пар» (no hold ≥ prompt) → existing
    // enterSteam() / 001 matrix. Independent of dcLatched / active DC1 —
    // reminder must never gate steam (same with or without DC1).
    clearDescaleHoldGesture();
    lastOledMs = 0;
    enterSteam();
    return;
  }

  if (elapsed < DESCALE_HOLD_ENTER_MS) {
    cancelDescaleHoldToEspresso();
    return;
  }

  // elapsed ≥ ENTER: commit PREP if tick lagged; R3 MUST NOT cancel (FR-004a).
  enterDescalePrep();
}

// Per-state tick from loop().
// PI → activeTargetC у loop (espresso pot | 120/128 steam); cutoff → FAULT (T032).
void updateMachineState(unsigned long now) {
  switch (machineState) {
    case ESPRESSO_HEAT:
      // FR-019: heat idle — помпа завжди PUMP_OFF_PCT, pot A0 ігнорується
      setPumpPercent(PUMP_OFF_PCT);
      // FR-002 / SC-001: Tt ≥ target − READY_BAND_C for READY_HOLD_MS → HE0
      // (no upper bound; overshoot OK)
      if (espressoReadyCriterion(now)) {
        sendToPanel("HE0");
        machineState = ESPRESSO_READY;
        resetReadyHold();
      }
      break;
    case ESPRESSO_READY:
      // FR-019: ready idle — помпа завжди PUMP_OFF_PCT, pot A0 ігнорується
      setPumpPercent(PUMP_OFF_PCT);
      break;
    case COLD_FLUSH:
      // FR-004 / SC-002 / T066: COLD_FLUSH_PCT for COLD_FLUSH_MS; ТЕН OFF у
      // updateHeaterControl. Timeout → P10 / ESPRESSO_READY (early B1 = T067).
      setTargetMode(TARGET_ESPRESSO);
      setPumpPercent(COLD_FLUSH_PCT);
      if (now >= brewPhaseStartMs &&
          (now - brewPhaseStartMs) >= COLD_FLUSH_MS) {
        stopColdFlush();
      }
      break;
    case BREW_2:
      // FR-006 / T023: PREINFUSE → PAUSE → FLOW @ pot
      setTargetMode(TARGET_ESPRESSO);
      updateBrewPhases(now);
      break;
    case STEAM_HEAT:
      // FR-019: heat idle — помпа PUMP_OFF_PCT
      // FR-013 / T032 / T080: PI → STEAM_READY_TARGET_C (120) до HS0 / cancel
      setPumpPercent(PUMP_OFF_PCT);
      setTargetMode(TARGET_STEAM);  // activeTargetC = STEAM_READY_TARGET_C
      // FR-011 / SC-005 / T043 / T080: Tt ≥ STEAM_READY_TARGET_C → HS0 одразу
      // (без READY_HOLD_MS, без верхньої смуги)
      // FR-026 / T045: якщо ручка вже відкрита — одразу resume SS1 / STEAM_FLOW @ 128
      if (steamReadyCriterion()) {
        sendToPanel("HS0");
        machineState = STEAM_READY;
        if (isSteamHandleOpen()) {
          sendToPanel("SS1");
          machineState = STEAM_FLOW;
          activeTargetC = STEAM_FLOW_TARGET_C;
          steamPulsePhase = STEAM_PULSE_PHASE_ON;
          steamPulsePhaseStartMs = now;
          setPumpPercent(STEAM_PULSE_PCT);
        }
      }
      break;
    case STEAM_READY:
      // FR-019: steam-ready idle — помпа PUMP_OFF_PCT
      // FR-013 / T032 / T046 / T080: PI на STEAM_READY_TARGET_C (120) до B1/B2 exit
      setPumpPercent(PUMP_OFF_PCT);
      setTargetMode(TARGET_STEAM);  // activeTargetC = STEAM_READY_TARGET_C
      // FR-026 / T057 / T080: auto-HS1 ONLY when handle closed AND
      // Tt < STEAM_READY_TARGET_C. Open handle → SS1 / STEAM_FLOW @ 128.
      // STEAM_FLOW has no auto-HS1 on temp drop (T056).
      if (!isSteamHandleOpen() && !steamReadyCriterion()) {
        sendToPanel("HS1");
        machineState = STEAM_HEAT;
        break;
      }
      // FR-012 / SC-006 / T029 / T080: handle open → SS1 / STEAM_FLOW @ 128
      if (isSteamHandleOpen()) {
        sendToPanel("SS1");
        machineState = STEAM_FLOW;
        activeTargetC = STEAM_FLOW_TARGET_C;
        // FR-012 / T030: start pulse ON immediately
        steamPulsePhase = STEAM_PULSE_PHASE_ON;
        steamPulsePhaseStartMs = now;
        setPumpPercent(STEAM_PULSE_PCT);
        break;
      }
      // FR-014 / SC-007 / T071: NO idle auto-exit → LT1 (T049 path removed).
      // STEAM_IDLE_EXIT_MS=0 (T064); вихід з пару лише B1/B2 → coolOrReady() (T070).
      break;
    case STEAM_FLOW:
      // T081 / FR-013 / FR-026 / SC-006: while handle open keep PI @
      // STEAM_FLOW_TARGET_C (128). Tt drop below 128 (or even < 120) MUST
      // stay in STEAM_FLOW — continuous pulses, steam-flow PI toward 128.
      // MUST NOT send HS1/SS0 or pause pump solely due to temp drop
      // (T048 removed by T056; dual-target confirm after T080).
      // No auto-HS1 here (that path is STEAM_READY + handle closed only — T057).
      setTargetMode(TARGET_STEAM);  // → STEAM_FLOW_TARGET_C while state==STEAM_FLOW
      // FR-015 / SC-006 / T047 / T080: handle close → SS0, pump 0,
      // restore STEAM_READY_TARGET_C (120) → STEAM_READY
      // (MUST NOT coolOrReady; no idle→LT1 — T071)
      if (!isSteamHandleOpen()) {
        setPumpPercent(PUMP_OFF_PCT);
        sendToPanel("SS0");
        machineState = STEAM_READY;
        setTargetMode(TARGET_STEAM);  // → STEAM_READY_TARGET_C after leave FLOW
        break;
      }
      // Intentionally no Tt / steamReadyCriterion branch — temp drop ≠ exit.
      // FR-012 / SC-006: STEAM_PULSE_ON_MS @ STEAM_PULSE_PCT / STEAM_PULSE_OFF_MS @ 0
      if (steamPulsePhase == STEAM_PULSE_PHASE_ON) {
        setPumpPercent(STEAM_PULSE_PCT);
        if ((now - steamPulsePhaseStartMs) >= STEAM_PULSE_ON_MS) {
          steamPulsePhase = STEAM_PULSE_PHASE_OFF;
          steamPulsePhaseStartMs = now;
          setPumpPercent(PUMP_OFF_PCT);
        }
      } else {
        setPumpPercent(PUMP_OFF_PCT);
        if ((now - steamPulsePhaseStartMs) >= STEAM_PULSE_OFF_MS) {
          steamPulsePhase = STEAM_PULSE_PHASE_ON;
          steamPulsePhaseStartMs = now;
          setPumpPercent(STEAM_PULSE_PCT);
        }
      }
      break;
    case STEAM_COOL:
      // FR-028 / SC-011 / T073: ТЕН FORCED OFF; target = espresso pot;
      // handle open → STEAM_PULSE_* @ STEAM_PULSE_PCT; closed → pump 0;
      // ready → pump 0 even if handle open → LT0 → ESPRESSO_READY.
      // B3→enterSteam / B1·B2 block — onPanelButton (T050/T068).
      setTargetMode(TARGET_ESPRESSO);
      heaterPercent = 0;
      heaterIntegral = 0.0f;
      digitalWrite(HEATER_PIN, LOW);
      heaterGateOn = false;

      // Ready first: stop pulse even if handle still open (FR-028, SC-011).
      // Cool criterion (upper band) — not HE0 one-sided lower.
      if (espressoCoolReadyCriterion(now)) {
        setPumpPercent(PUMP_OFF_PCT);
        sendToPanel("LT0");
        machineState = ESPRESSO_READY;
        resetReadyHold();
        break;
      }

      if (isSteamHandleOpen()) {
        // Same STEAM_PULSE_ON_MS / OFF @ STEAM_PULSE_PCT as STEAM_FLOW.
        if (steamPulsePhase == STEAM_PULSE_PHASE_ON) {
          setPumpPercent(STEAM_PULSE_PCT);
          if ((now - steamPulsePhaseStartMs) >= STEAM_PULSE_ON_MS) {
            steamPulsePhase = STEAM_PULSE_PHASE_OFF;
            steamPulsePhaseStartMs = now;
            setPumpPercent(PUMP_OFF_PCT);
          }
        } else {
          setPumpPercent(PUMP_OFF_PCT);
          if ((now - steamPulsePhaseStartMs) >= STEAM_PULSE_OFF_MS) {
            steamPulsePhase = STEAM_PULSE_PHASE_ON;
            steamPulsePhaseStartMs = now;
            setPumpPercent(STEAM_PULSE_PCT);
          }
        }
      } else {
        setPumpPercent(PUMP_OFF_PCT);
        // Next open starts ON immediately (same as STEAM_READY→FLOW entry).
        steamPulsePhase = STEAM_PULSE_PHASE_ON;
        steamPulsePhaseStartMs = now;
      }
      break;
    case DESCALE_HOLD: {
      // T012 / FR-003 / FR-006a countdown: espresso PI stays (heater path);
      // pump gated off (pumpMayRun). OLED countdown after PROMPT_MS.
      setPumpPercent(PUMP_OFF_PCT);

      if (!descaleHoldGesture.active) {
        break;
      }

      const unsigned long elapsed = now - descaleHoldGesture.pressMs;

      if (elapsed >= DESCALE_HOLD_PROMPT_MS) {
        if (!descaleHoldGesture.countdownVisible) {
          descaleHoldGesture.countdownVisible = true;
          lastDescaleCountdownSec = 0xFFFF;
          lastOledMs = 0;
        }
        const unsigned int sec = descaleHoldCountdownSec(now);
        if (sec != lastDescaleCountdownSec) {
          lastDescaleCountdownSec = sec;
          lastOledMs = 0;  // refresh «через N сек» on second change
        }
      }

      // T013 / FR-001, FR-004: elapsed ≥ ENTER → PREP regardless of dcLatched.
      if (elapsed >= DESCALE_HOLD_ENTER_MS) {
        enterDescalePrep();
      }
      break;
    }
    case DESCALE_PREP:
      // T013 / FR-004: pump 0; heater force-off (descaleHeaterForceOff);
      // OLED PREP / HANDLE_CLOSED via updateOled. New B3 → onPanelButton (T014).
      setPumpPercent(PUMP_OFF_PCT);
      break;
    case DESCALE_REFILL:
      // T021 / FR-014a: pump 0; heater force-off (descaleHeaterForceOff);
      // OLED REFILL / HANDLE_CLOSED via updateOled. New B3 → onPanelButton.
      // No auto-timeout abort — wait indefinitely for refill B3.
      setPumpPercent(PUMP_OFF_PCT);
      break;
    case DESCALE_WAIT_RINSE:
      // T022 / FR-014a: pump 0; heater force-off (descaleHeaterForceOff);
      // OLED WAIT_RINSE / HANDLE_CLOSED via updateOled. New B3 → onPanelButton.
      // No auto-timeout abort — wait indefinitely for rinse B3.
      setPumpPercent(PUMP_OFF_PCT);
      break;
    case DESCALE_REFILL_WATER:
      // T025 / FR-012, FR-014a: pump 0; heater force-off (descaleHeaterForceOff);
      // OLED REFILL_WATER / HANDLE_CLOSED via updateOled; phasePulses kept;
      // no DC0 / DC1 stays. New B3 → onPanelButton. No auto-timeout abort.
      setPumpPercent(PUMP_OFF_PCT);
      break;
    case DESCALE_PHASE1: {
      // T017 / FR-007, FR-006: ON DESCALE_PHASE1_ON_MS @ DESCALE_PUMP_PCT /
      // OFF DESCALE_PHASE1_OFF_MS @ PUMP_OFF_PCT via millis() (no delay());
      // PI holds DESCALE_PHASE1_TEMP_C through ON and OFF (descaleHeaterSetpoint).
      // T018 / FR-009: accumulate phasePulses; ml via FLOW_PULSES_PER_ML.
      // T019 / FR-010: no-flow armed only during ON → REFILL if vol < target.
      // T020 / FR-011: volume ≥ target → WAIT_RINSE (priority over refill).
      // T028 / US4: isSteamHandleOpen() MUST NOT stop pulses or force
      // wait/success/refill — only volume / no-flow / FAULT (hydraulic path).
      float sp = 0.0f;
      if (descaleHeaterSetpoint(&sp)) {
        activeTargetC = sp;
      }

      // T018: fold ISR → phasePulses; stamp lastFlowPulseMs on new pulses.
      accumulateDescalePhasePulses();

      // T020 / FR-011: volume ≥ target (incl. no-flow ON when already ≥ target)
      // → WAIT_RINSE; priority over refill. No DC0 / DecalcTimer clear (FR-014).
      if (descalePhaseVolumeReached()) {
        enterDescaleWaitRinse();
        break;
      }

      if (descaleSession.pulsePhase == DESCALE_PULSE_PHASE_ON &&
          descaleSession.noFlowArmed &&
          (now - descaleSession.lastFlowPulseMs) >= DESCALE_PHASE1_NOFLOW_MS) {
        // T019 / FR-010: early empty → REFILL; keep pulses + DC1.
        enterDescaleRefill();
        break;
      }

      if (descaleSession.pulsePhase == DESCALE_PULSE_PHASE_ON) {
        setPumpPercent(DESCALE_PUMP_PCT);
        descaleSession.noFlowArmed = true;  // T019: arm only during ON
        if ((now - descaleSession.pulsePhaseStartedMs) >= DESCALE_PHASE1_ON_MS) {
          descaleSession.pulsePhase = DESCALE_PULSE_PHASE_OFF;
          descaleSession.pulsePhaseStartedMs = now;
          descaleSession.noFlowArmed = false;  // T019: pause no-flow on OFF
          setPumpPercent(PUMP_OFF_PCT);
        }
      } else {
        setPumpPercent(PUMP_OFF_PCT);
        descaleSession.noFlowArmed = false;
        if ((now - descaleSession.pulsePhaseStartedMs) >= DESCALE_PHASE1_OFF_MS) {
          // T019 / FR-010: pause across OFF — restore ON-only elapsed so
          // DESCALE_PHASE1_NOFLOW_MS can span multiple ON slices (defaults
          // ON=500 < NOFLOW=1000). Hard reset would never trip with defaults.
          const unsigned long offStarted = descaleSession.pulsePhaseStartedMs;
          if (descaleSession.lastFlowPulseMs <= offStarted) {
            const unsigned long elapsedAtOff =
                offStarted - descaleSession.lastFlowPulseMs;
            descaleSession.lastFlowPulseMs = now - elapsedAtOff;
          } else {
            descaleSession.lastFlowPulseMs = now;
          }
          descaleSession.pulsePhase = DESCALE_PULSE_PHASE_ON;
          descaleSession.pulsePhaseStartedMs = now;
          descaleSession.noFlowArmed = true;
          setPumpPercent(DESCALE_PUMP_PCT);
        }
      }
      break;
    }
    case DESCALE_PHASE2: {
      // T023 / FR-008, FR-006: ON DESCALE_PHASE2_ON_MS @ DESCALE_PUMP_PCT /
      // OFF DESCALE_PHASE2_OFF_MS @ PUMP_OFF_PCT via millis() (no delay());
      // heater force-off entire phase 2 (descaleHeaterForceOff / updateHeaterControl).
      // T024 / FR-012, FR-012a: accumulate phasePulses; no-flow armed only during
      // ON → REFILL_WATER if vol < target (no DC0); volume ≥ target → success stub.
      // OLED PHASE2_RUN via updateOled.
      // T028 / US4: isSteamHandleOpen() MUST NOT stop pulses or force
      // wait/success/refill — only volume / no-flow / FAULT (hydraulic path).

      // T024: fold ISR → phasePulses; stamp lastFlowPulseMs on new pulses.
      accumulateDescalePhasePulses();

      // T024 / T026 / FR-012a, FR-013: volume ≥ target (incl. no-flow ON when
      // already ≥ target) → success (DC0 clear + coolOrReady); priority over refill.
      if (descalePhaseVolumeReached()) {
        completeDescalePhase2Success();
        break;
      }

      if (descaleSession.pulsePhase == DESCALE_PULSE_PHASE_ON &&
          descaleSession.noFlowArmed &&
          (now - descaleSession.lastFlowPulseMs) >= DESCALE_PHASE2_NOFLOW_MS) {
        // T024 / FR-012: early empty → REFILL_WATER; keep pulses + DC1; no DC0.
        enterDescaleRefillWater();
        break;
      }

      if (descaleSession.pulsePhase == DESCALE_PULSE_PHASE_ON) {
        setPumpPercent(DESCALE_PUMP_PCT);
        descaleSession.noFlowArmed = true;  // T024: arm only during ON
        if ((now - descaleSession.pulsePhaseStartedMs) >= DESCALE_PHASE2_ON_MS) {
          descaleSession.pulsePhase = DESCALE_PULSE_PHASE_OFF;
          descaleSession.pulsePhaseStartedMs = now;
          descaleSession.noFlowArmed = false;  // T024: pause no-flow on OFF
          setPumpPercent(PUMP_OFF_PCT);
        }
      } else {
        setPumpPercent(PUMP_OFF_PCT);
        descaleSession.noFlowArmed = false;
        if ((now - descaleSession.pulsePhaseStartedMs) >= DESCALE_PHASE2_OFF_MS) {
          // T024 / FR-012: pause across OFF — restore ON-only elapsed so
          // DESCALE_PHASE2_NOFLOW_MS can span multiple ON slices (defaults
          // ON=500 < NOFLOW=1500). Hard reset would never trip with defaults.
          const unsigned long offStarted = descaleSession.pulsePhaseStartedMs;
          if (descaleSession.lastFlowPulseMs <= offStarted) {
            const unsigned long elapsedAtOff =
                offStarted - descaleSession.lastFlowPulseMs;
            descaleSession.lastFlowPulseMs = now - elapsedAtOff;
          } else {
            descaleSession.lastFlowPulseMs = now;
          }
          descaleSession.pulsePhase = DESCALE_PULSE_PHASE_ON;
          descaleSession.pulsePhaseStartedMs = now;
          descaleSession.noFlowArmed = true;
          setPumpPercent(DESCALE_PUMP_PCT);
        }
      }
      break;
    }
    case FAULT:
      // FR-018: latch until power cycle — heater OFF, pump 0; OLED via updateOled;
      // no transition out (buttons ignored; coolOrReady refuses FAULT).
      enterFault();
      break;
  }
}

void handlePanelByte(char c) {
  if (c == '\r') {
    return;
  }

  if (c == '\n') {
    if (linkLen > 0) {
      linkBuf[linkLen] = '\0';

      // Ingress ≤ LINK_CODE_MAX_LEN: B1/B2/B3 press + R3 «пар» release
      // (contracts/panel-hold-uart.md). Unknown codes → USB log only.
      if (linkLen == 2 && linkBuf[0] == 'B' &&
          (linkBuf[1] == '1' || linkBuf[1] == '2' || linkBuf[1] == '3')) {
        onPanelButton(linkBuf[1]);
      } else if (linkLen == 2 && linkBuf[0] == 'R' && linkBuf[1] == '3') {
        onPanelSteamRelease();
      } else {
        Serial.print(F("← panel (unknown) "));
        Serial.println(linkBuf);
      }

      linkLen = 0;
    }
    return;
  }

  if (linkLen < LINK_CODE_MAX_LEN) {
    linkBuf[linkLen++] = c;
  } else {
    // Overflow beyond LINK_CODE_MAX_LEN — discard frame
    linkLen = 0;
  }
}

void pollPanelUart() {
  while (panel.available() > 0) {
    handlePanelByte((char)panel.read());
  }
}

// ---------------------------------------------------------------------------
// Decalc EEPROM + uptime (FR-016, FR-023, SC-008)
// ---------------------------------------------------------------------------
// True while pump/flow timing is SLA-sensitive — defer blocking EEPROM.put.
// Include STEAM_COOL: open-handle pulse + B3≤300 ms (SC-009/011) must not
// stall on AVR EEPROM.write during cool.
bool decalcEepromWriteWouldStallTransitions() {
  return machineState == COLD_FLUSH || machineState == BREW_2 ||
         machineState == STEAM_FLOW || machineState == STEAM_COOL;
}

void saveDecalcToEeprom() {
  if (decalcEepromWriteWouldStallTransitions()) {
    decalcEepromDirty = true;
    return;
  }

  DecalcEepromLayout rec;
  rec.magic = DECALC_EEPROM_MAGIC;
  rec.accumulatedOnSeconds = accumulatedOnSeconds;
  rec.dcLatched = dcLatched ? 1u : 0u;
  EEPROM.put(DECALC_EEPROM_ADDR, rec);
  decalcEepromDirty = false;
}

void loadDecalcFromEeprom() {
  // FR-014 / T027: power cycle restores latch + accumulator — never auto-DC0 /
  // never zero interval on boot (only first-boot / corrupt magic seeds empty).
  DecalcEepromLayout rec;
  EEPROM.get(DECALC_EEPROM_ADDR, rec);
  if (rec.magic != DECALC_EEPROM_MAGIC) {
    accumulatedOnSeconds = 0;
    dcLatched = false;
    saveDecalcToEeprom();  // seed valid block on first boot / corrupt magic
    return;
  }
  accumulatedOnSeconds = rec.accumulatedOnSeconds;
  dcLatched = (rec.dcLatched != 0);
  decalcEepromDirty = false;
}

// Non-blocking: accumulate powered-on seconds, flush EEPROM periodically,
// send one DC1 when interval reached and not yet latched.
// SC-008: never waits; never consults/gates MachineState; EEPROM deferred in brew/steam flow.
void updateDecalcUptime(unsigned long now) {
  const unsigned long elapsedMs = now - lastDecalcMs;
  lastDecalcMs = now;
  decalcMsCarry += elapsedMs;

  while (decalcMsCarry >= DECALC_MS_PER_S) {
    decalcMsCarry -= DECALC_MS_PER_S;
    if (accumulatedOnSeconds < 0xFFFFFFFFu) {
      accumulatedOnSeconds++;
    }

    secondsSinceDecalcSave++;
    if (secondsSinceDecalcSave >= DECALC_SAVE_EVERY_S) {
      secondsSinceDecalcSave = 0;
      saveDecalcToEeprom();
    }
  }

  // Latch + panel frame only — does not change MachineState (SC-008).
  // Also covers boot with EEPROM already past interval and latch cleared.
  if (!dcLatched && accumulatedOnSeconds >= DECALC_INTERVAL_S) {
    sendToPanel("DC1");
    dcLatched = true;
    secondsSinceDecalcSave = 0;
    saveDecalcToEeprom();
  }

  // Catch up deferred persist once brew/steam flow ends.
  if (decalcEepromDirty) {
    saveDecalcToEeprom();
  }
}

// ---------------------------------------------------------------------------
// USB Serial service (FR-023 DC0 clear; optional DC1 force for stand)
// ---------------------------------------------------------------------------
// Shared DecalcTimer clear (USB + phase-2 success only).
// FR-019 / T027: USB DC0 MUST still clear reminder without entering/running the
// descaling program (no MachineState change here). FR-013 success reuses this.
// FR-014: interrupt / FAULT / power cycle MUST NOT call this — only the two
// callers below (USB handleServiceLine, completeDescalePhase2Success).
void handleServiceDc0() {
  sendToPanel("DC0");
  dcLatched = false;
  accumulatedOnSeconds = 0;
  secondsSinceDecalcSave = 0;
  saveDecalcToEeprom();
  Serial.println(F("# service DC0: cleared latch + EEPROM accumulator"));
}

// Optional stand force: send DC1 and latch even if already latched.
void handleServiceDc1() {
  sendToPanel("DC1");
  dcLatched = true;
  secondsSinceDecalcSave = 0;
  saveDecalcToEeprom();
  Serial.println(F("# service DC1: forced latch"));
}

void handleServiceLine(const char *cmd) {
  // FR-019 / T027: USB "DC0\n" → clear only (same as 001); does not start descale.
  if (strcmp(cmd, "DC0") == 0) {
    handleServiceDc0();
    return;
  }
  if (strcmp(cmd, "DC1") == 0) {
    handleServiceDc1();
    return;
  }

  Serial.print(F("# service (unknown) "));
  Serial.println(cmd);
}

void handleServiceByte(char c) {
  if (c == '\r') {
    return;
  }

  if (c == '\n') {
    if (serviceLen > 0) {
      serviceBuf[serviceLen] = '\0';
      handleServiceLine(serviceBuf);
      serviceLen = 0;
    }
    return;
  }

  if (serviceLen < LINK_CODE_MAX_LEN) {
    serviceBuf[serviceLen++] = c;
  } else {
    // Overflow beyond LINK_CODE_MAX_LEN — discard frame
    serviceLen = 0;
  }
}

void pollUsbSerial() {
  while (Serial.available() > 0) {
    handleServiceByte((char)Serial.read());
  }
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setupUnusedPins() {
  // Входи штатної плати, які в цій прошивці нічого не роблять
  pinMode(SDD_PIN, INPUT_PULLUP);

  // Ручка пари (D10) — активний LOW через оптопару; API: isSteamHandleOpen()
  pinMode(STEAM_HANDLE_PIN, INPUT_PULLUP);

  // FLOWMETER_PIN — pinMode + PCINT у setupFlowmeterInterrupt()

  // D8/D9 — NeoSWSerial panel (TX/RX); pinMode тут не ставимо

  // Потенціометри — аналогові входи без підтяжки
  pinMode(PUMP_POT_PIN, INPUT);
  pinMode(TEMP_POT_PIN, INPUT);

  // CS термопари бачка (D11) НЕ читаємо, але зобов'язані тримати HIGH!
  // Обидва MAX6675 сидять на спільній шині SO (D12). Якщо CS другого чипа
  // лишити «плаваючим» входом, він може опинитися в LOW і тоді ОБИДВА модулі
  // одночасно виганяють дані на SO — шина конфліктує, і перший модуль читає
  // усі нулі → readCelsius() повертає рівно 0.0 (не NAN, бо біт обриву теж 0).
  pinMode(THERMO_CS_TANK, OUTPUT);
  digitalWrite(THERMO_CS_TANK, HIGH);
}

void setup() {
  // Після watchdog-reset бутлоадер може лишити WDT увімкненим → цикл
  // перезавантажень. Гасимо його й чистимо прапорці скидання.
  MCUSR = 0;
  wdt_disable();

  // Watchdog: якщо loop() зависне (I2C, Serial), МК перезавантажиться і
  // актуатори знову опиняться у безпечному стані (ТЕН LOW, помпа 0).
  wdt_enable(WDTO_2S);

  Serial.begin(PC_BAUD);
  panel.begin(LINK_BAUD);

  // ТЕН заблокований (LOW) ще до ініціалізації периферії
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  setupUnusedPins();
  setupFlowmeterInterrupt();
  resetFlowPulses();
  loadDecalcFromEeprom();

  // Диммер: Zero-Cross = D2 (всередині бібліотеки), GATE = D3.
  // Переривання вимкнені на час ініціалізації: інакше перший zero-cross може
  // прийти до того, як бібліотека отримає валідний pin (масив sort_vals
  // заповнюється лише в Dimmer_init_end).
  noInterrupts();
  Dimmer_init_begin();
  Dimmer_pin_assign(PUMP_DIMMER, DIMMER_GATE_PIN);
  Dimmer_init_end();
  Dimm_value(PUMP_DIMMER, 0);
  interrupts();
  pumpDimmerLevel = 0;

  // OLED: шина I2C. Adafruit_SSD1306 сама викликає Wire.begin() і тримає
  // 400 кГц під час передачі кадру (128×32 ≈ 512 байт ≈ 12 мс).
  displayOk = display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR);
  if (!displayOk) {
    Serial.println(F("# OLED init failed"));
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 4);
    display.print(F("EC685: PI heater"));
    display.setCursor(0, 16);
    display.print(F("set 20..100 C, A1"));
    display.display();
  }

  delay(MAX6675_BOOT_DELAY_MS);  // перша конверсія MAX6675 після живлення

  Serial.println(F("# EC685 main brew logic + panel UART"));
  Serial.println(F("# USB service: DC0 (clear); optional DC1 (force latch)"));
  Serial.println(F("# t_ms,Tt_C,temp_valid,set_C,heater_pct,heater_on,pump_cmd_pct,dimmer_0_255,steam_h,state"));

  const unsigned long now = millis();
  lastBlockTempMs = now;
  lastPotMs = now;
  lastControlMs = now;
  lastOledMs = now;
  lastSerialMs = now;
  heaterWindowStartMs = now;
  lastDecalcMs = now;
  decalcMsCarry = 0;
  secondsSinceDecalcSave = 0;

  tempBlockC = thermoT.readCelsius();
  tempValid = !isnan(tempBlockC) && tempBlockC > TEMP_VALID_MIN_C &&
              tempBlockC < TEMP_VALID_MAX_C;

  // Стартова позиція потенціометрів — одразу, без плавного «розгону» з нуля
  pumpPercentFiltered = (float)readPotAverage(PUMP_POT_PIN) * 100.0f / 1023.0f;
  potPumpPercent = (uint8_t)(PUMP_MIN_PCT +
                              (PUMP_MAX_PCT - PUMP_MIN_PCT) * pumpPercentFiltered / 100.0f +
                              0.5f);
  setPumpPercent(PUMP_OFF_PCT);  // authority default: OFF поза brew/steam flow

  targetPercentFiltered = (float)readPotAverage(TEMP_POT_PIN) * 100.0f / 1023.0f;
  espressoTargetC =
      TARGET_MIN_C + (TARGET_MAX_C - TARGET_MIN_C) * targetPercentFiltered / 100.0f;
  setTargetMode(TARGET_ESPRESSO);

  // ТЕН на старті вимкнений; ПІ вийде на потрібну потужність за перші 100 мс
  digitalWrite(HEATER_PIN, LOW);
  heaterIntegral = 0.0f;
  heaterPercent = 0;
  resetReadyHold();

  // Power-on → ESPRESSO_HEAT (немає COLD_START): activeTargetC уже з espresso pot (setTargetMode вище)
  machineState = ESPRESSO_HEAT;
  sendToPanel("HE1");  // FR-001/FR-002: одразу на панель
}

void loop() {
  wdt_reset();

  // 0. Panel UART RX + USB service — до знімка `now`, щоб startBrew* /
  // brewPhaseStartMs=millis() не опинився пізніше за `now` і не дав
  // unsigned underflow у updateBrewPhases (миттєвий skip PREINFUSE).
  pollPanelUart();
  pollUsbSerial();

  const unsigned long now = millis();

  // 1. Термопара термоблоку — джерело зворотного зв'язку для регулятора
  updateTemperature(now);

  // 2. Потенціометри: A0 → potPumpPercent, A1 → espressoTargetC / activeTargetC
  updatePots(now);

  // 3. FSM tick — espresso/steam + DESCALE_* stubs (T006); US1–US3 fill handlers
  updateMachineState(now);

  // 4. Pump authority: поза brew/steam flow завжди PUMP_OFF_PCT
  enforcePumpAuthority();

  // 5. ПІ → цільова потужність ТЕНу (activeTargetC)
  updateHeaterControl(now);

  // 6. Часове ШІМ на SSR — КОЖЕН цикл, щоб фронт не квантувався до 100 мс
  updateHeaterGate(now);

  // 7. Decalc uptime (FR-016, SC-008): accumulate / periodic EEPROM / one DC1
  updateDecalcUptime(now);

  // 8. Вивід
  updateOled(now);
  logSerial(now);
}

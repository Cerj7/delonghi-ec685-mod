// Main Brew Logic — 013_main_brew_logic.cpp
//
// База: scripts/006_thermoblock_pi_heater.cpp (+ NeoSWSerial, EEPROM).
// Цільова прошивка основної Nano: еспресо / пролив / пар / декальцинація
// (specs/001-main-brew-logic/). Heater/sensor/OLED/WDT/unused pins — з 006
// через Tunables (SAFETY_CUTOFF_C=130 для пару). Panel UART + USB DC0/DC1 service;
// MachineState + shared helpers (T009: targets, ready hold, pump authority, fault;
// T011: coolOrReady).
//
// Мета (006, до портування FSM):
//   1. Потенціометр температури (A1) задає цільову температуру термоблоку:
//      від кімнатної / холодного стану (20 °C) до 100 °C — максимум.
//   2. ПІ-регулятор за термопарою термоблоку (MAX6675 #1, CS = A2) керує
//      SSR ТЕНу (D4, HIGH = нагрів) часовим ШІМ з вікном 1 с.
//   3. На OLED: Tt — температура термоблоку, P — ПОТОЧНА потужність ТЕНу (%),
//      дрібним шрифтом S — задана температура.
//   4. Потенціометр помпи (A0) керує диммером помпи — так само, як у 005.
//      На екран потужність помпи НЕ виводиться (лише в Serial).
//   5. Кнопки не зчитуються і нічого не запускають.
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
//   A0  потенціометр помпи (0…100 %)
//   A1  потенціометр температури термоблоку (20…100 °C)
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
// Усі бізнес °C / мс / % / періоди тут. У тілі FSM/loop — лише імена констант
// (допустимі 0/1 для булевої логіки, індекси циклів, розміри буферів протоколу).
// =============================================================================

// --- Temperatures (°C) ---
// Потенціометр A1: проти годинникової → TARGET_MIN_C (холодний стан / нагрів OFF),
// за годинниковою → TARGET_MAX_C. FR-001.
const float TARGET_MIN_C = 30.0f;       // Низький край потенціометра еспресо
const float TARGET_MAX_C = 110.0f;      // Верхній край потенціометра еспресо
const float TARGET_OFF_BAND_C = 1.0f;   // Біля мінімуму = нагрів OFF (не тримати 20 °C)
const float STEAM_TARGET_C = 130.0f;    // Ціль пару (FR-009, FR-013)
// FR-011: HS0 = Tt ≥ STEAM_TARGET_C (без верхньої смуги). STEAM_READY_BAND_C не
// використовується для HS0; залишено для сумісності (див. contracts/tunable-constants.md).
const float STEAM_READY_BAND_C = 0.0f;
const float READY_BAND_C = 1.0f;        // ± допуск готовності еспресо / після LT1 (FR-002)
const float HEATER_DEADBAND_C = 0.5f;   // Мертва зона ПІ (інтеграл «замерзає»)
// Аварійний поріг перегріву термоблоку (FR-013 / FR-018 / T032): якщо Tt ≥ цього
// значення → ТЕН OFF, помпа 0, OLED «FAULT», скидання лише power cycle.
// Має бути строго вище STEAM_TARGET_C, інакше пар одразу потрапить у FAULT.
const float SAFETY_CUTOFF_C = 145.0f;
const float TEMP_VALID_MIN_C = 1.0f;    // Нижня правдоподібність термопари
const float TEMP_VALID_MAX_C = 200.0f;  // Верхня правдоподібність термопари

// --- Timing (ms unless noted) ---
const unsigned long READY_HOLD_MS = 1000;            // Утримання в смузі перед HE0/LT0 (FR-002, SC-001)
const unsigned long PREINFUSION_MS = 2000;           // Тривалість преінфузії (FR-004, SC-002)
const unsigned long PREINFUSION_PAUSE_MS = 2000;     // Пауза після преінфузії (FR-004, SC-002)
const unsigned long STEAM_PULSE_ON_MS = 110;         // Імпульс помпи пару ON (SC-006)
const unsigned long STEAM_PULSE_OFF_MS = 880;       // Імпульс помпи пару OFF (SC-006)
// FR-014 / SC-007: idle у HS0 (ручка закрита, без кнопок) → LT1. 0 = вимкнено.
// Дефолт 10000 мс — contracts/tunable-constants.md.
const unsigned long STEAM_IDLE_EXIT_MS = 10000;
const unsigned long HEATER_WINDOW_MS = 1000;         // Вікно часового ШІМ ТЕНу
const unsigned long CONTROL_INTERVAL_MS = 100;       // Період перерахунку ПІ
const unsigned long BLOCK_TEMP_INTERVAL_MS = 250;    // Опитування MAX6675 (≥220 мс)
const unsigned long POT_INTERVAL_MS = 20;            // Опитування потенціометрів
const unsigned long OLED_INTERVAL_MS = 200;          // Оновлення OLED (поза brew ≥2 Гц)
const unsigned long BREW_DISPLAY_INTERVAL_MS = 500;  // Оновлення brew-дисплея (SC-004)
const unsigned long SERIAL_INTERVAL_MS = 500;        // Періодичний USB-лог
const unsigned long WATCHDOG_TIMEOUT_MS = 2000;      // WDT (апаратно: WDTO_2S)
const unsigned long BUTTON_REACT_MAX_MS = 300;       // Цільовий SLA зупинки (SC-003/SC-006; тест)
const unsigned long TIMING_TOLERANCE_MS = 200;       // Допуск стендових перевірок (SC-002/SC-007)

// --- Powers (%) ---
const uint8_t PREINFUSION_PCT = 80;   // Преінфузія (FR-004, SC-002)
const uint8_t STEAM_PULSE_PCT = 100;  // Імпульс пари (SC-006)
const uint8_t PUMP_OFF_PCT = 0;       // Явний «стоп» помпи

// --- PI / pots / dimmer ---
// Kp: %/°C; Ki: %/(°C·с); Ti = Kp/Ki ≈ 100 с. Калібрувати docs/thermoblock-pi-heater.md.
// Три профілі (FR-027 / SC-010, stand → C): base · brew-flow (4× + FF) · steam-flow (2.5× + FF).
const float HEATER_KP = 5.0f;         // Базовий ПІ Kp
const float HEATER_KI = 0.05f;        // Базовий ПІ Ki
const float HEATER_KP_BREW = 25.0f;   // Brew-flow Kp (4× base) — лише BrewPhase==FLOW (FR-027)
const float HEATER_KI_BREW = 0.25f;   // Brew-flow Ki (4× base) (FR-027)
const uint8_t HEATER_FF_BREW_PCT = 25;  // Адитивний FF % під час brew FLOW (FR-027)
const float HEATER_KP_STEAM = 20.0f;  // Steam-flow Kp (2.5× base) — увесь STEAM_FLOW (FR-027)
const float HEATER_KI_STEAM = 0.20f; // Steam-flow Ki (2.5× base) (FR-027)
const uint8_t HEATER_FF_STEAM_PCT = 20; // Адитивний FF % під час STEAM_FLOW (FR-027)
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

// --- Link / buffers (contracts/panel-uart.md; як у 012_main_panel_link.cpp) ---
// D9 RX ← D6 панелі, D8 TX → D5 панелі, 8N1 @ LINK_BAUD; USB debug @ PC_BAUD.
const uint8_t PANEL_RX_PIN = 9;
const uint8_t PANEL_TX_PIN = 8;
const long LINK_BAUD = 19200;
const long PC_BAUD = 115200;
const uint8_t LINK_CODE_MAX_LEN = 3;  // Макс. довжина коду панелі (напр. P11)

// --- Flow display ---
// 0 = показувати імпульси флоуметра; >0 = мл (імпульси / FLOW_PULSES_PER_ML).
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
// potPumpPercent — зчитане з A0 (для FLOW); pumpCommandPct — фактична команда.
uint8_t potPumpPercent = 0;
uint8_t pumpCommandPct = PUMP_OFF_PCT;
uint8_t pumpDimmerLevel = 0;  // фактично застосований рівень диммера
float pumpPercentFiltered = 0.0f;

// --- Стан: уставки (data-model TemperatureSetpoint) ---
enum TargetMode : uint8_t {
  TARGET_ESPRESSO = 0,
  TARGET_STEAM,
};

TargetMode targetMode = TARGET_ESPRESSO;
float espressoTargetC = TARGET_MIN_C;  // з A1 (TARGET_MIN_C…TARGET_MAX_C)
float activeTargetC = TARGET_MIN_C;    // espressoTargetC або STEAM_TARGET_C
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
  BREW_1,
  BREW_2,
  STEAM_HEAT,
  STEAM_READY,
  STEAM_FLOW,
  STEAM_COOL,
  FAULT,
};

MachineState machineState = ESPRESSO_HEAT;

// --- Brew profile phase (data-model BrewProfile; FR-004) ---
enum BrewPhase : uint8_t {
  BREW_PHASE_PREINFUSE = 0,
  BREW_PHASE_PAUSE,
  BREW_PHASE_FLOW,
};

BrewPhase brewPhase = BREW_PHASE_PREINFUSE;
unsigned long brewPhaseStartMs = 0;

// Idle clock for STEAM_READY (HS0): handle closed → LT1 after STEAM_IDLE_EXIT_MS.
unsigned long steamReadySinceMs = 0;

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

// WaterCounter.pulses — імпульси за поточний пролив; reset на старті BREW_*.
volatile uint32_t flowPulses = 0;

// --- DecalcTimer (data-model.md / research §7; FR-016, FR-023, SC-008) ---
// EEPROM layout @ DECALC_EEPROM_ADDR: magic | accumulatedOnSeconds | dcLatched.
// Non-blocking reminder only: dcLatched MUST NEVER gate brew/heat/steam FSM.
// EEPROM.flush deferred during BREW_*/STEAM_FLOW so AVR EEPROM write (~ms/byte)
// cannot stall pump/heater timing (SC-008 / constitution §III).
const int DECALC_EEPROM_ADDR = 0;
const unsigned long DECALC_MS_PER_S = 1000UL;  // ms → whole uptime seconds

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

// WaterCounter.reset — викликати на вході в BREW_1 / BREW_2.
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

// Pump authority (contracts/main-fsm.md): non-zero лише в BREW_* / STEAM_FLOW.
// Усі інші стани → PUMP_OFF_PCT незалежно від pot A0.
bool pumpMayRun() {
  return machineState == BREW_1 || machineState == BREW_2 || machineState == STEAM_FLOW;
}

void enforcePumpAuthority() {
  if (!pumpMayRun() && pumpCommandPct != PUMP_OFF_PCT) {
    setPumpPercent(PUMP_OFF_PCT);
  } else if (!pumpMayRun() && pumpDimmerLevel != 0) {
    setPumpPercent(PUMP_OFF_PCT);
  }
}

// ---------------------------------------------------------------------------
// Уставки / ready / fault (data-model.md, contracts/main-fsm.md)
// ---------------------------------------------------------------------------
void setTargetMode(TargetMode mode) {
  targetMode = mode;
  activeTargetC = (mode == TARGET_STEAM) ? STEAM_TARGET_C : espressoTargetC;
}

void refreshActiveTarget() {
  activeTargetC = (targetMode == TARGET_STEAM) ? STEAM_TARGET_C : espressoTargetC;
}

void resetReadyHold() {
  readyBandLatched = false;
  readyBandSinceMs = 0;
}

// Espresso ready / LT0.
// ESPRESSO_HEAT (HE0):
//   - Tt ≥ target → одразу готово (увімкнення з уже гарячим термоблоком; інакше
//     при Tt > target+band HE0 не прийде, доки не охолоне в ±band).
//   - target−band ≤ Tt < target → класичний hold READY_HOLD_MS.
// STEAM_COOL (LT0): |Tt − target| ≤ band протягом READY_HOLD_MS.
bool espressoReadyCriterion(unsigned long now) {
  if (!tempValid || isnan(tempBlockC)) {
    resetReadyHold();
    return false;
  }

  if (machineState == ESPRESSO_HEAT) {
    if (tempBlockC >= espressoTargetC) {
      resetReadyHold();
      return true;
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

  // STEAM_COOL / інші виклики: симетрична смуга + hold.
  if (fabs(tempBlockC - espressoTargetC) <= READY_BAND_C) {
    if (!readyBandLatched) {
      readyBandLatched = true;
      readyBandSinceMs = now;
    }
    return (now - readyBandSinceMs) >= READY_HOLD_MS;
  }

  resetReadyHold();
  return false;
}

// Steam HS0: Tt ≥ STEAM_TARGET_C — миттєво, без hold і без верхньої смуги (FR-011, SC-005).
bool steamReadyCriterion() {
  if (!tempValid || isnan(tempBlockC)) {
    return false;
  }
  return tempBlockC >= STEAM_TARGET_C;
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

// Latched FAULT: heater OFF, pump 0, OLED fault. Exit v1 = power cycle only.
// T022 / quickstart S6: abort active BREW_1 — P10 so panel has no dangling P11.
void enterFault() {
  if (machineState == FAULT) {
    heaterPercent = 0;
    heaterIntegral = 0.0f;
    digitalWrite(HEATER_PIN, LOW);
    heaterGateOn = false;
    setPumpPercent(PUMP_OFF_PCT);
    return;
  }

  // Abort brew before latching: clear portion LED (P11→P10), then pump 0.
  if (machineState == BREW_1) {
    sendToPanel("P10");
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

  // --- Помпа A0 → 0…100 % (лише зчитування; диммер — через setPumpPercent / authority) ---
  const float pumpPercentRaw = (float)readPotAverage(PUMP_POT_PIN) * 100.0f / 1023.0f;

  pumpPercentFiltered += (pumpPercentRaw - pumpPercentFiltered) * POT_SMOOTHING;
  if (pumpPercentFiltered < 0.0f) {
    pumpPercentFiltered = 0.0f;
  }
  if (pumpPercentFiltered > 100.0f) {
    pumpPercentFiltered = 100.0f;
  }
  potPumpPercent = (uint8_t)(pumpPercentFiltered + 0.5f);

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
// вимкнення квантувався б до 100 мс, і на сталій потужності 6 % реально було б
// 10 % (100 мс із 1000) — регулятор не міг би тримати ціль точно.
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
// FR-013 / T032: у TARGET_STEAM ПІ тримає activeTargetC=STEAM_TARGET_C (не pot A1)
// до виходу з пару (coolOrReady / idle LT1). FR-018: невалідна Tt або
// Tt ≥ SAFETY_CUTOFF_C → ТЕН OFF (+ FAULT на cutoff / streak у updateTemperature).
void updateHeaterControl(unsigned long now) {
  if (now - lastControlMs < CONTROL_INTERVAL_MS) {
    return;
  }
  const unsigned long dtMs = now - lastControlMs;
  lastControlMs = now;

  // Нагрів вимкнено: FAULT, ручка в мінімумі (лише espresso), термопара, cutoff.
  // У всіх цих випадках інтеграл скидається, щоб після повернення в робочий
  // режим не було «пострілу» накопиченою потужністю.
  if (machineState == FAULT) {
    heaterIntegral = 0.0f;
    heaterPercent = 0;
    return;
  }

  // TARGET_OFF_BAND стосується лише еспресо: у парі pot A1 не гасить ТЕН (FR-013).
  const bool espressoOffBand =
      targetMode == TARGET_ESPRESSO &&
      espressoTargetC <= TARGET_MIN_C + TARGET_OFF_BAND_C;
  const bool heaterAllowed = tempValid && !espressoOffBand;
  // FR-018 / T032: SAFETY_CUTOFF_C захисний і під час утримання пару (~120 °C).
  if (!heaterAllowed || tempBlockC >= SAFETY_CUTOFF_C) {
    heaterIntegral = 0.0f;
    heaterPercent = 0;
    if (tempValid && tempBlockC >= SAFETY_CUTOFF_C) {
      enterFault();
    }
    return;
  }

  // FR-027 / SC-010: профіль кожен CONTROL_INTERVAL_MS (≤ 1 тік).
  // brew-flow лише (BREW_1|BREW_2) && FLOW; steam-flow лише STEAM_FLOW; інакше base.
  // FF адитивний лише під час flow; без target-offset; SAFETY_CUTOFF_C спільний (вище).
  float kp = HEATER_KP;
  float ki = HEATER_KI;
  float ffPct = 0.0f;
  if ((machineState == BREW_1 || machineState == BREW_2) &&
      brewPhase == BREW_PHASE_FLOW) {
    kp = HEATER_KP_BREW;
    ki = HEATER_KI_BREW;
    ffPct = (float)HEATER_FF_BREW_PCT;
  } else if (machineState == STEAM_FLOW) {
    kp = HEATER_KP_STEAM;
    ki = HEATER_KI_STEAM;
    ffPct = (float)HEATER_FF_STEAM_PCT;
  }

  const float error = activeTargetC - tempBlockC;
  const float dt = (float)dtMs / 1000.0f;
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
// FR-008 / SC-004 / T021: brew layout — water counter, Tt, pump % @ BREW_DISPLAY_INTERVAL_MS.
void showBrewOled() {
  if (!displayOk) {
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);

  const uint32_t pulses = getFlowPulses();

  // Рядок 1 — лічильник води (імпульси або мл через FLOW_PULSES_PER_ML).
  display.setCursor(0, 0);
  display.print(F("W="));
  if (FLOW_PULSES_PER_ML > 0.0f) {
    display.print(flowDisplayValue(pulses), 1);
    display.print(F("ml"));
  } else {
    display.print(pulses);
  }

  // Рядок 2 — температура термоблоку.
  display.setCursor(0, 10);
  display.print(F("Tt="));
  if (isnan(tempBlockC)) {
    display.print(F("---"));
  } else {
    display.print(tempBlockC, 1);
    display.print(F("C"));
  }

  // Рядок 3 — поточна потужність помпи (%); OPEN = ручка пари.
  display.setCursor(0, 20);
  display.print(F("Pp="));
  display.print(pumpCommandPct);
  display.print(F("%"));
  if (isSteamHandleOpen()) {
    display.print(F(" OPEN"));
  }

  display.display();
}

void updateOled(unsigned long now) {
  // T021 / T025 / FR-008: BREW_1 and BREW_2 share showBrewOled @ BREW_DISPLAY_INTERVAL_MS.
  const bool brewDisplay = (machineState == BREW_1 || machineState == BREW_2);
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

  if (brewDisplay) {
    showBrewOled();
    return;
  }

  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);

  // Рядок 1 (великий) — температура термоблоку.
  // «!» у кінці = показання невалідне (обрив, конфлікт на шині) → ТЕН OFF.
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.print(F("Tt="));
  if (isnan(tempBlockC)) {
    display.print(F("---"));
  } else {
    display.print(tempBlockC, 1);
    display.print(F("C"));
  }
  if (!tempValid) {
    display.print(F("!"));
  }

  // Рядок 2 (великий) — поточна потужність ТЕНу
  display.setCursor(0, 16);
  display.print(F("P="));
  display.print(heaterPercent);
  display.print(F("%"));

  // Рядок 3 (дрібний): зліва стан пару / OPEN ручки; справа уставка.
  display.setTextSize(1);
  display.setCursor(0, 24);
  if (isSteamHandleOpen()) {
    // Завжди видно відкриту ручку — і для подачі, і для діагностики полярності D10.
    display.print(F("OPEN"));
  } else if (machineState == STEAM_HEAT) {
    display.print(F("HS"));
  } else if (machineState == STEAM_READY) {
    display.print(F("rdy"));
  } else if (machineState == STEAM_FLOW) {
    display.print(F("flow"));
  } else if (machineState == STEAM_COOL) {
    display.print(F("cool"));
  }

  char setText[8];
  snprintf(setText, sizeof(setText), "S=%d", (int)(activeTargetC + 0.5f));
  const int16_t setWidth = (int16_t)strlen(setText) * 6;
  display.setCursor(SCREEN_WIDTH - setWidth, 24);
  display.print(setText);

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

// After steam cancel (B3 @ HS1/HS0) or idle STEAM_IDLE_EXIT_MS: espresso pot
// target, then cool or heat/ready. MUST NOT call from SS0 / auto-reheat.
// contracts/main-fsm.md — coolOrReady()
void coolOrReady() {
  // FR-018: FAULT latched until power cycle — never auto-recover via coolOrReady
  if (machineState == FAULT) {
    return;
  }

  setTargetMode(TARGET_ESPRESSO);

  const bool hotterThanEspresso =
      tempValid && !isnan(tempBlockC) && (tempBlockC > espressoTargetC);
  if (hotterThanEspresso) {
    if (machineState != STEAM_COOL) {
      sendToPanel("LT1");
    }
    machineState = STEAM_COOL;
    resetReadyHold();
    return;
  }

  sendToPanel("LT0");

  // Invalid/unknown Tt → reheat; strict below setpoint → HE1; else already at/above → ready.
  const bool colderThanEspresso =
      !tempValid || isnan(tempBlockC) || (tempBlockC < espressoTargetC);
  if (colderThanEspresso) {
    sendToPanel("HE1");
    machineState = ESPRESSO_HEAT;
    resetReadyHold();
    return;
  }

  machineState = ESPRESSO_READY;
  resetReadyHold();
}

// FR-004 / T017: B1 @ ESPRESSO_READY → P11, BREW_1, counter reset, PREINFUSE.
void startBrew1() {
  sendToPanel("P11");
  machineState = BREW_1;
  resetFlowPulses();
  brewPhase = BREW_PHASE_PREINFUSE;
  brewPhaseStartMs = millis();
  setPumpPercent(PREINFUSION_PCT);
}

// FR-005 / SC-003 / T019: repeat B1 @ BREW_1 → pump off, P10, ESPRESSO_READY
// (цільовий бюджет реакції: BUTTON_REACT_MAX_MS).
void stopBrew1() {
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
  // FR-018: FAULT latched until power cycle — never enter steam from fault
  if (machineState == FAULT) {
    return;
  }

  setTargetMode(TARGET_STEAM);  // activeTargetC = STEAM_TARGET_C

  // FR-009 / FR-011: Tt ≥ STEAM_TARGET_C → HS0 immediately (no band/hold); else HS1.
  if (steamReadyCriterion()) {
    sendToPanel("HS0");
    machineState = STEAM_READY;
    steamReadySinceMs = millis();  // reset idle clock
    return;
  }

  sendToPanel("HS1");
  machineState = STEAM_HEAT;
}

// FR-004 / SC-002 / T018: shared brew profile — PREINFUSE → PAUSE → FLOW.
// During PREINFUSE/PAUSE ignore pot A0; FLOW tracks live potPumpPercent.
// Guard: if brewPhaseStartMs was stamped with millis() after loop's `now`
// (B1/B2 handled earlier in the same iteration), unsigned (now - start)
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
// Central button entry: framing delivers only B1/B2/B3 as '1'/'2'/'3'.
// Bodies stay empty until US handlers; ignore paths match the contract matrix.
void onPanelButton(char code) {
  // FR-003 / matrix: ESPRESSO_HEAT + FAULT — no pump, mode, or indication side effects
  if (machineState == ESPRESSO_HEAT || machineState == FAULT) {
    return;
  }

  switch (machineState) {
    case ESPRESSO_READY:
      if (code == '1') {
        startBrew1();
      } else if (code == '2') {
        startBrew2();
      } else if (code == '3') {
        // FR-009 / T044: B3 → enterSteam() (HS0 if already hot, else HS1)
        enterSteam();
      }
      break;

    case BREW_1:
      // FR-005 / T020: only B1 stops; B2/B3 — no pump/mode/indication side effects
      if (code == '1') {
        stopBrew1();
      }
      break;

    case BREW_2:
      // FR-007 / T024: only B2 stops; B1/B3 — no pump/mode/indication side effects
      if (code == '2') {
        stopBrew2();
      }
      break;

    case STEAM_HEAT:
      // FR-010 / T028: B3 @ HS1 → cancel steam via coolOrReady()
      if (code == '3') {
        coolOrReady();
      }
      // FR-022 / T031: B1/B2 ignore in all steam/cool states
      break;

    case STEAM_READY:
      // FR-021 / T029: B3 @ HS0 → coolOrReady() immediately; MUST NOT SS1
      if (code == '3') {
        coolOrReady();
      } else {
        // FR-014 / T049: any button event breaks inactivity → reset idle clock
        // (FR-022: B1/B2 still do not change mode / pump / indication)
        steamReadySinceMs = millis();
      }
      break;

    case STEAM_FLOW:
      // FR-022: ignore B1/B2; ignore B3 only during SS1 / STEAM_FLOW
      break;

    case STEAM_COOL:
      // FR-025 / SC-009 / T050: B3 → enterSteam() (interrupt cool; HS0 if hot else HS1)
      // FR-022: B1/B2 still ignored in cool
      if (code == '3') {
        enterSteam();
      }
      break;

    case ESPRESSO_HEAT:
    case FAULT:
      // unreachable: early return above
      break;
  }
}

// Per-state tick from loop().
// PI → activeTargetC у loop (espresso pot | STEAM_TARGET_C); cutoff → FAULT (T032).
void updateMachineState(unsigned long now) {
  switch (machineState) {
    case ESPRESSO_HEAT:
      // FR-019: heat idle — помпа завжди PUMP_OFF_PCT, pot A0 ігнорується
      setPumpPercent(PUMP_OFF_PCT);
      // FR-002 / SC-001: hold in READY_BAND_C for READY_HOLD_MS → HE0 / ESPRESSO_READY
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
    case BREW_1:
      // Spec assumption / T022: during brew ТЕН keeps espresso pot (A1) target —
      // setTargetMode + updatePots→espressoTargetC→activeTargetC; PI in loop.
      setTargetMode(TARGET_ESPRESSO);
      // FR-004 / SC-002: PREINFUSION_MS @ PREINFUSION_PCT → pause → FLOW @ pot
      updateBrewPhases(now);
      break;
    case BREW_2:
      // FR-006 / T023: same profile as BREW_1 via shared updateBrewPhases
      setTargetMode(TARGET_ESPRESSO);
      updateBrewPhases(now);
      break;
    case STEAM_HEAT:
      // FR-019: heat idle — помпа PUMP_OFF_PCT
      // FR-013 / T032: PI → STEAM_TARGET_C у loop (activeTargetC), до HS0 / cancel
      setPumpPercent(PUMP_OFF_PCT);
      setTargetMode(TARGET_STEAM);
      // FR-011 / SC-005 / T043: Tt ≥ STEAM_TARGET_C → HS0 одразу (без READY_HOLD_MS, без верхньої смуги)
      // FR-026 / T045: якщо ручка вже відкрита — одразу resume SS1 / STEAM_FLOW
      if (steamReadyCriterion()) {
        sendToPanel("HS0");
        machineState = STEAM_READY;
        steamReadySinceMs = now;
        if (isSteamHandleOpen()) {
          sendToPanel("SS1");
          machineState = STEAM_FLOW;
          steamPulsePhase = STEAM_PULSE_PHASE_ON;
          steamPulsePhaseStartMs = now;
          setPumpPercent(STEAM_PULSE_PCT);
        }
      }
      break;
    case STEAM_READY:
      // FR-019: steam-ready idle — помпа PUMP_OFF_PCT
      // FR-013 / T032 / T046: PI на STEAM_TARGET_C (ручка open/closed) до idle/cancel
      setPumpPercent(PUMP_OFF_PCT);
      setTargetMode(TARGET_STEAM);
      // FR-026 / T057: auto-HS1 ONLY when handle closed AND Tt < STEAM_TARGET_C.
      // Open handle falls through to SS1 / STEAM_FLOW (never reheat-pause from ready).
      // STEAM_FLOW has no auto-HS1 on temp drop (T056).
      if (!isSteamHandleOpen() && !steamReadyCriterion()) {
        sendToPanel("HS1");
        machineState = STEAM_HEAT;
        break;
      }
      // FR-012 / SC-006 / T029: handle open → SS1 / STEAM_FLOW (priority over idle;
      // FR-014 / T049: open handle / STEAM_FLOW pauses idle; close restarts clock)
      if (isSteamHandleOpen()) {
        sendToPanel("SS1");
        machineState = STEAM_FLOW;
        // FR-012 / T030: start pulse ON immediately
        steamPulsePhase = STEAM_PULSE_PHASE_ON;
        steamPulsePhaseStartMs = now;
        setPumpPercent(STEAM_PULSE_PCT);
        break;
      }
      // FR-014 / SC-007 / T049: count STEAM_IDLE_EXIT_MS only with handle closed
      // and no button events (B1/B2 reset steamReadySinceMs in onPanelButton).
      // On expiry → same cool path as cancel (coolOrReady → LT1/STEAM_COOL or
      // LT0). STEAM_IDLE_EXIT_MS==0 → авто-вихід вимкнено (лише B3 / ручка).
      if (STEAM_IDLE_EXIT_MS > 0 &&
          (now - steamReadySinceMs) >= STEAM_IDLE_EXIT_MS) {
        coolOrReady();
      }
      break;
    case STEAM_FLOW:
      // FR-013 / T032: під час SS1 імпульсів теж тримати STEAM_TARGET_C
      // (steam-flow PI уже вибрано в updateHeaterPi — T054)
      setTargetMode(TARGET_STEAM);
      // FR-015 / SC-006 / T047: handle close → SS0, pump 0 → STEAM_READY + idle
      // (MUST NOT coolOrReady)
      if (!isSteamHandleOpen()) {
        setPumpPercent(PUMP_OFF_PCT);
        sendToPanel("SS0");
        machineState = STEAM_READY;
        steamReadySinceMs = now;  // start/continue idle timer after flow
        break;
      }
      // FR-026 / SC-006 / T056: Tt < STEAM_TARGET_C → STAY in STEAM_FLOW.
      // MUST NOT send HS1/SS0 or pause pump solely due to temp drop (T048 removed).
      // Pulse continues below; heater keeps steam-flow PI toward STEAM_TARGET_C.
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
      // FR-014 / FR-019 / T031: cool idle — pump 0; target = espresso pot; PI in loop
      setPumpPercent(PUMP_OFF_PCT);
      setTargetMode(TARGET_ESPRESSO);
      // FR-014: same ready criterion as HE0 → LT0 → ESPRESSO_READY
      if (espressoReadyCriterion(now)) {
        sendToPanel("LT0");
        machineState = ESPRESSO_READY;
        resetReadyHold();
      }
      break;
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

      if (linkLen == 2 && linkBuf[0] == 'B' &&
          (linkBuf[1] == '1' || linkBuf[1] == '2' || linkBuf[1] == '3')) {
        onPanelButton(linkBuf[1]);
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
bool decalcEepromWriteWouldStallTransitions() {
  return machineState == BREW_1 || machineState == BREW_2 ||
         machineState == STEAM_FLOW;
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
// USB DC0 → panel DC0, clear latch, zero accumulator, persist (data-model DecalcTimer).
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

  delay(500);  // перша конверсія MAX6675 після живлення

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
  potPumpPercent = (uint8_t)(pumpPercentFiltered + 0.5f);
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

  // 3. FSM tick (stubs until US handlers)
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

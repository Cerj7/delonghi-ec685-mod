// Thermoblock PI Heater + Pump + Display — 006_thermoblock_pi_heater.cpp
//
// Мета:
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
//   D5  флоуметр — не використовується
//   D6  штатний SDD — не використовується
//   D8  Кнопка 1 вхід — не використовується (вхід з підтяжкою)
//   D9  Кнопка 1 вихід — не використовується (вхід; ШІМ тут не використовувати)
//   D10 ручка пари — не використовується
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
//
// Опис, формат Serial, методика налаштування ПІ — docs/thermoblock-pi-heater.md

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <max6675.h>
#include <AC_Dimmer.h>
#include <avr/wdt.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
#define BTN_READ_PIN 8   // не використовується
#define BTN_DRIVE_PIN 9  // не використовується
#define STEAM_HANDLE_PIN 10

#define PUMP_POT_PIN A0
#define TEMP_POT_PIN A1
#define PUMP_DIMMER 0  // номер каналу диммера в AC_Dimmer (від 0)

// --- Задана температура ---
// Потенціометр A1: повністю проти годинникової → 20 °C (кімнатна, «холодний
// стан»: нагрів вимкнений), повністю за годинниковою → 100 °C.
const float TARGET_MIN_C = 20.0f;
const float TARGET_MAX_C = 100.0f;
// Запас навколо мінімуму: якщо ручка ближче — вважаємо «нагрів OFF» і не
// тримаємо 20 °C активно (інакше в холодній кімнаті ТЕН грів би до 20 °C).
const float TARGET_OFF_BAND_C = 1.0f;

// --- Потужність помпи ---
// Частка потужності (%) → рівень диммера AC_Dimmer (0…240).
// Диммер ріже фазу: рівень — це затримка відкриття симистора після zero-cross
// (у бібліотеці: t = (19850 − 77·level) · 0.5 мкс, тобто 0…≈9.9 мс на півперіоді
// 10 мс), тому рівень НЕ пропорційний потужності (див. docs/dimmer.md).
// Таблиця — обернена RMS-крива для синусоїди: level дає таку частку діючої
// напруги (а отже й потужності на активному навантаженні).
// 100 % = 240 — максимум, який приймає бібліотека.
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

// --- ПІ-регулятор ТЕНу ---
// Часове ШІМ: на термоблоці стоїть SSR, тому перемикання раз на секунду
// безпечне. Менше вікно → точніше дозування; 1 с — компроміс для теплової
// інерції термоблоку (десятки секунд).
const unsigned long HEATER_WINDOW_MS = 1000;
const unsigned long CONTROL_INTERVAL_MS = 100;

// Kp: % потужності на градус похибки.
// Ki: % на градус за секунду (час інтегрування Ti = Kp / Ki = 100 с).
// Стартові значення підібрані симуляцією теплової моделі (1350 Вт, 350–1100 г,
// затримка датчика 3 с): перерегулювання ≤2 °C, вихід на ціль ~45 с з 22 °C.
// Калібрувати за docs/thermoblock-pi-heater.md.
const float HEATER_KP = 5.0f;
const float HEATER_KI = 0.05f;

// Мертва зона: усередині неї інтеграл «замерзає» (пропорційна складова
// лишається), тому шум ±0.25 °C від MAX6675 не накопичується в інтеграторі.
const float HEATER_DEADBAND_C = 0.5f;

// Апаратний захист: вище цієї температури ТЕН вимикається, інтеграл скидається.
const float SAFETY_CUTOFF_C = 105.0f;

// Межі правдоподібності для термопари типу K на термоблоці. Рівно 0.0 при
// живій термопарі неможливе (кімната 20–25 °C) — це ознака конфлікту на шині.
const float TEMP_VALID_MIN_C = 1.0f;
const float TEMP_VALID_MAX_C = 200.0f;
const uint8_t TEMP_INVALID_STREAK_LIMIT = 3;

// --- Періоди ---
// MAX6675: ≥220 мс після read() того самого чипа.
const unsigned long BLOCK_TEMP_INTERVAL_MS = 250;
const unsigned long POT_INTERVAL_MS = 20;
const unsigned long OLED_INTERVAL_MS = 200;
const unsigned long SERIAL_INTERVAL_MS = 500;

// Потенціометри: 8 усереднених відліків + експоненційне згладжування.
// Без цього АЦП-шум смикає % на екрані, фазу на симисторі й цільову T.
const uint8_t POT_SAMPLES = 8;
const float POT_SMOOTHING = 0.3f;  // 0…1: менше — плавніше, більше — швидше
// Новий рівень застосовується, лише якщо відрізняється від поточного на стільки
const uint8_t DIMMER_HYSTERESIS = 2;

// --- Стан: термопара ---
float tempBlockC = NAN;
bool tempValid = false;
uint8_t tempInvalidStreak = 0;
uint8_t zeroTempStreak = 0;

// --- Стан: помпа ---
uint8_t pumpPercent = 0;      // згладжене значення для Serial
uint8_t pumpDimmerLevel = 0;  // фактично застосований рівень диммера
float pumpPercentFiltered = 0.0f;

// --- Стан: ТЕН ---
float targetTempC = TARGET_MIN_C;    // задана температура з A1
float targetPercentFiltered = 0.0f;  // згладжена позиція ручки, 0…100
uint8_t heaterPercent = 0;           // вихід ПІ = середня потужність за вікно, %
float heaterIntegral = 0.0f;         // інтегральна складова, %
bool heaterGateOn = false;           // фактичний стан виходу D4
unsigned long heaterWindowStartMs = 0;

bool displayOk = false;

unsigned long lastBlockTempMs = 0;
unsigned long lastPotMs = 0;
unsigned long lastControlMs = 0;
unsigned long lastOledMs = 0;
unsigned long lastSerialMs = 0;

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
  if (delta >= DIMMER_HYSTERESIS || delta <= -DIMMER_HYSTERESIS) {
    setPumpLevel(level);
  }
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

  // --- Помпа: A0 → 0…100 % → диммер ---
  const float pumpPercentRaw = (float)readPotAverage(PUMP_POT_PIN) * 100.0f / 1023.0f;

  pumpPercentFiltered += (pumpPercentRaw - pumpPercentFiltered) * POT_SMOOTHING;
  if (pumpPercentFiltered < 0.0f) {
    pumpPercentFiltered = 0.0f;
  }
  if (pumpPercentFiltered > 100.0f) {
    pumpPercentFiltered = 100.0f;
  }
  pumpPercent = (uint8_t)(pumpPercentFiltered + 0.5f);
  applyPumpPercent(pumpPercent);

  // --- Температура: A1 → 20…100 °C ---
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
  targetTempC =
      TARGET_MIN_C + (TARGET_MAX_C - TARGET_MIN_C) * targetPercentFiltered / 100.0f;
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
void updateHeaterControl(unsigned long now) {
  if (now - lastControlMs < CONTROL_INTERVAL_MS) {
    return;
  }
  const unsigned long dtMs = now - lastControlMs;
  lastControlMs = now;

  // Нагрів вимкнено: ручка в мінімумі, термопара невалідна, перегрів.
  // У всіх цих випадках інтеграл скидається, щоб після повернення в робочий
  // режим не було «пострілу» накопиченою потужністю.
  const bool heaterAllowed = tempValid && targetTempC > TARGET_MIN_C + TARGET_OFF_BAND_C;
  if (!heaterAllowed || tempBlockC >= SAFETY_CUTOFF_C) {
    heaterIntegral = 0.0f;
    heaterPercent = 0;
    return;
  }

  const float error = targetTempC - tempBlockC;
  const float dt = (float)dtMs / 1000.0f;
  const float p = HEATER_KP * error;

  // Інтегратор: у мертвій зоні «замерзає» — шум термопари не накопичується.
  if (fabs(error) >= HEATER_DEADBAND_C) {
    float iNext = heaterIntegral + HEATER_KI * error * dt;
    if (iNext < 0.0f) {
      iNext = 0.0f;
    }
    if (iNext > 100.0f) {
      iNext = 100.0f;
    }

    // Anti-windup: якщо вихід у насиченні й похибка «розганяє» його далі —
    // інтеграл не накопичуємо (інакше після виходу з насичення буде перегрів).
    const float raw = p + iNext;
    if ((raw > 100.0f && error > 0.0f) || (raw < 0.0f && error < 0.0f)) {
      iNext = heaterIntegral;
    }
    heaterIntegral = iNext;
  }

  float output = p + heaterIntegral;
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
    if (tempValid) {
      Serial.println(F("# WARN термопара невалідна — ТЕН вимкнено"));
    }
    tempValid = false;
  }
}

// ---------------------------------------------------------------------------
// OLED
// ---------------------------------------------------------------------------
void updateOled(unsigned long now) {
  if (!displayOk || now - lastOledMs < OLED_INTERVAL_MS) {
    return;
  }
  lastOledMs = now;

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

  // Рядок 2 (дрібний, праворуч) — задана температура з потенціометра A1
  char setText[8];
  snprintf(setText, sizeof(setText), "S=%d", (int)(targetTempC + 0.5f));
  display.setTextSize(1);
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

  // t_ms,Tt_C,temp_valid,set_C,heater_pct,heater_on,pump_pct,dimmer_0_255
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
  Serial.print(targetTempC, 1);
  Serial.print(',');
  Serial.print(heaterPercent);
  Serial.print(',');
  Serial.print(heaterGateOn ? 1 : 0);
  Serial.print(',');
  Serial.print(pumpPercent);
  Serial.print(',');
  Serial.println(pumpDimmerLevel);
}

// ---------------------------------------------------------------------------
// setup / loop
// ---------------------------------------------------------------------------
void setupUnusedPins() {
  // Входи штатної плати, які в цій прошивці нічого не роблять
  pinMode(SDD_PIN, INPUT_PULLUP);
  pinMode(FLOWMETER_PIN, INPUT_PULLUP);
  pinMode(STEAM_HANDLE_PIN, INPUT_PULLUP);

  // Кнопки не зчитуються: лінії лишаємо входами з підтяжкою,
  // Arduino їх не штовхає і не сканує
  pinMode(BTN_READ_PIN, INPUT_PULLUP);
  pinMode(BTN_DRIVE_PIN, INPUT_PULLUP);

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

  Serial.begin(115200);

  // ТЕН заблокований (LOW) ще до ініціалізації периферії
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  setupUnusedPins();

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

  Serial.println(F("# EC685 thermoblock PI heater + pump (buttons ignored)"));
  Serial.println(F("# t_ms,Tt_C,temp_valid,set_C,heater_pct,heater_on,pump_pct,dimmer_0_255"));

  const unsigned long now = millis();
  lastBlockTempMs = now;
  lastPotMs = now;
  lastControlMs = now;
  lastOledMs = now;
  lastSerialMs = now;
  heaterWindowStartMs = now;

  tempBlockC = thermoT.readCelsius();
  tempValid = !isnan(tempBlockC) && tempBlockC > TEMP_VALID_MIN_C &&
              tempBlockC < TEMP_VALID_MAX_C;

  // Стартова позиція потенціометрів — одразу, без плавного «розгону» з нуля
  pumpPercentFiltered = (float)readPotAverage(PUMP_POT_PIN) * 100.0f / 1023.0f;
  pumpPercent = (uint8_t)(pumpPercentFiltered + 0.5f);
  setPumpLevel(powerPercentToDimmerLevel(pumpPercent));

  targetPercentFiltered = (float)readPotAverage(TEMP_POT_PIN) * 100.0f / 1023.0f;
  targetTempC =
      TARGET_MIN_C + (TARGET_MAX_C - TARGET_MIN_C) * targetPercentFiltered / 100.0f;

  // ТЕН на старті вимкнений; ПІ вийде на потрібну потужність за перші 100 мс
  digitalWrite(HEATER_PIN, LOW);
  heaterIntegral = 0.0f;
  heaterPercent = 0;
}

void loop() {
  wdt_reset();
  const unsigned long now = millis();

  // 1. Термопара термоблоку — джерело зворотного зв'язку для регулятора
  updateTemperature(now);

  // 2. Потенціометри: A0 → помпа, A1 → задана температура
  updatePots(now);

  // 3. ПІ → цільова потужність ТЕНу
  updateHeaterControl(now);

  // 4. Часове ШІМ на SSR — КОЖЕН цикл, щоб фронт не квантувався до 100 мс
  updateHeaterGate(now);

  // 5. Вивід
  updateOled(now);
  logSerial(now);
}

// Pot Pump + Thermo Display — 006_pot_pump_temp_display.cpp
//
// Мета:
//   1. Потенціометр помпи (A0) напряму задає потужність помпи через AC-диммер.
//   2. На OLED — температура термоблоку (термопара #1) і ПОТОЧНА потужність помпи.
//   3. ТЕН НЕ вмикається взагалі: D4 завжди LOW.
//   4. Кнопки не зчитуються і нічого не запускають.
//   5. Watchdog 2 с: зависання → reset → актуатори в безпечному стані.
//   6. CS другої термопари (бачок, D11) не читається, але тримається HIGH —
//      інакше конфлікт на спільній шині SO і Tt читається як 0.0.
//
// Піни — за pinout у readme.md (після перенесення термопар):
//   D2  Zero-Cross диммера (фіксовано бібліотекою AC_Dimmer, INT0)
//   D3  GATE диммера → симистор помпи
//   D4  SSR ТЕНу — завжди LOW (нагрів заблокований)
//   D5  флоуметр — не використовується
//   D6  штатний SDD — не використовується
//   D8  Кнопка 1 вхід — не використовується (вхід з підтяжкою)
//   D9  Кнопка 1 вихід — не використовується (вхід; ШИМ тут не використовувати)
//   D10 ручка пари — не використовується
//   A2  CS термопари #1 — термоблок / вихід   ← читаємо і показуємо
//   D11 CS термопари #2 — бачок води          ← не читаємо, але тримаємо HIGH!
//   D12 SO / D13 SCK — спільна шина MAX6675
//   A0  потенціометр помпи (0…100 %)
//   A1  потенціометр температури — не використовується
//   A4 / A5 — OLED I2C
//
// Бібліотеки (Менеджер бібліотек Arduino IDE):
//   1. Adafruit GFX Library
//   2. Adafruit SSD1306
//   3. MAX6675 library (by Adafruit)
//   4. AC_dimmer-dmr-0.2.1 (zip з https://github.com/serhiy23412/AC_dimmer-dmr-0.2.1)
//
// Опис, формат Serial і діагностика — docs/pot-pump-temp-display.md

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <max6675.h>
#include <AC_Dimmer.h>
#include <avr/wdt.h>

// --- OLED 0.91" 128x32 I2C (SDA=A4, SCL=A5) ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define OLED_I2C_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Термопара термоблоку / виходу (MAX6675 #1, CS = A2) ---
#define THERMO_SO 12
#define THERMO_SCK 13
#define THERMO_CS_BLOCK A2
// CS другого модуля (бачок). Не читаємо, але тримати HIGH ОБОВ'ЯЗКОВО —
// див. setup().
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
#define PUMP_DIMMER 0  // номер каналу диммера в AC_Dimmer (від 0)

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

// --- Періоди ---
// MAX6675: ≥220 мс після read() того самого чипа.
const unsigned long BLOCK_TEMP_INTERVAL_MS = 250;
const unsigned long POT_INTERVAL_MS = 20;
const unsigned long OLED_INTERVAL_MS = 200;
const unsigned long SERIAL_INTERVAL_MS = 500;

// Потенціометр: 8 усереднених відліків + експоненційне згладжування.
// Без цього АЦП-шум смикає % на екрані й фазу на симисторі.
const uint8_t POT_SAMPLES = 8;
const float POT_SMOOTHING = 0.3f;  // 0…1: менше — плавніше, більше — швидше
// Новий рівень застосовується, лише якщо відрізняється від поточного на стільки
const uint8_t DIMMER_HYSTERESIS = 2;

// --- Стан ---
float tempBlockC = NAN;
uint8_t zeroTempStreak = 0;

uint8_t pumpPercent = 0;      // згладжене значення для екрана й Serial
uint8_t pumpDimmerLevel = 0;  // фактично застосований рівень диммера
float pumpPercentFiltered = 0.0f;

bool displayOk = false;

unsigned long lastBlockTempMs = 0;
unsigned long lastPotMs = 0;
unsigned long lastOledMs = 0;
unsigned long lastSerialMs = 0;

// ---------------------------------------------------------------------------
// Помпа
// ---------------------------------------------------------------------------
// AC_Dimmer пише в спільний масив, який читає ISR zero-cross. Виклик робимо
// атомарно, щоб ISR не побачив напівоновлене значення.
void setPumpLevel(uint8_t level) {
  noInterrupts();
  Dimm_value(PUMP_DIMMER, level);
  interrupts();
  pumpDimmerLevel = level;
}

void updatePumpFromPot(unsigned long now) {
  if (now - lastPotMs < POT_INTERVAL_MS) {
    return;
  }
  lastPotMs = now;

  uint16_t sum = 0;
  for (uint8_t i = 0; i < POT_SAMPLES; i++) {
    sum += analogRead(PUMP_POT_PIN);
  }
  const float average = (float)sum / POT_SAMPLES;
  const float percent = average * 100.0f / 1023.0f;

  pumpPercentFiltered += (percent - pumpPercentFiltered) * POT_SMOOTHING;
  if (pumpPercentFiltered < 0.0f) {
    pumpPercentFiltered = 0.0f;
  }
  pumpPercent = (uint8_t)(pumpPercentFiltered + 0.5f);

  const uint8_t level = powerPercentToDimmerLevel(pumpPercent);
  const int16_t delta = (int16_t)level - (int16_t)pumpDimmerLevel;
  if (delta >= DIMMER_HYSTERESIS || delta <= -DIMMER_HYSTERESIS) {
    setPumpLevel(level);
  }
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
  if (!isnan(tempBlockC) && tempBlockC < 0.1f) {
    if (zeroTempStreak < 255) {
      zeroTempStreak++;
    }
    if (zeroTempStreak == 4) {
      Serial.println(F("# WARN Tt=0.0 — перевір CS термопари бачка (D11) = HIGH "
                       "і CS термоблоку (A2)"));
    }
    return;
  }
  zeroTempStreak = 0;
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
  display.setTextSize(2);

  // Рядок 1 — термоблок / вихід
  display.setCursor(0, 0);
  display.print(F("Tt="));
  if (isnan(tempBlockC)) {
    display.print(F("---"));
  } else {
    display.print(tempBlockC, 1);
    display.print(F("C"));
  }

  // Рядок 2 — поточна потужність помпи
  display.setCursor(0, 16);
  display.print(F("P="));
  display.print(pumpPercent);
  display.print(F("%"));

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

  // t_ms,Tt_C,pump_pct,dimmer_0_255
  Serial.print(now);
  Serial.print(',');
  if (isnan(tempBlockC)) {
    Serial.print(F("nan"));
  } else {
    Serial.print(tempBlockC, 2);
  }
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

  // Потенціометр температури не використовується
  pinMode(A1, INPUT);
  // Потенціометр помпи — аналоговий вхід без підтяжки
  pinMode(PUMP_POT_PIN, INPUT);

  // CS термопари бачка (D11) НЕ читаємо, але зобов'язані тримати HIGH!
  // Обидва MAX6675 сидять на спільній шині SO (D12). Якщо CS другого чипа
  // лишити «плаваючим» входом, він може опинитися в LOW і тоді ОБИДВА модулі
  // одночасно виганяють дані на SO — шина конфліктує, і перший модуль читає
  // усі нулі → readCelsius() повертає рівно 0.0 (не NAN, бо біт обриву теж 0).
  // Саме тому в 004_dual_thermocouple.cpp це працює: конструктор MAX6675
  // другого модуля сам робить pinMode(CS, OUTPUT) + digitalWrite(CS, HIGH).
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

  // ТЕН заблокований ще до ініціалізації периферії
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
    display.print(F("EC685: pot + thermo"));
    display.setCursor(0, 16);
    display.print(F("heater OFF"));
    display.display();
  }

  delay(500);  // перша конверсія MAX6675 після живлення

  Serial.println(F("# EC685 pot-pump + thermoblock display (heater permanently OFF)"));
  Serial.println(F("# t_ms,Tt_C,pump_pct,dimmer_0_255"));

  const unsigned long now = millis();
  lastBlockTempMs = now;
  lastPotMs = now;
  lastOledMs = now;
  lastSerialMs = now;

  tempBlockC = thermoT.readCelsius();

  // Стартова позиція потенціометра — одразу без плавного «розгону» з нуля
  uint16_t sum = 0;
  for (uint8_t i = 0; i < POT_SAMPLES; i++) {
    sum += analogRead(PUMP_POT_PIN);
  }
  pumpPercentFiltered = ((float)sum / POT_SAMPLES) * 100.0f / 1023.0f;
  pumpPercent = (uint8_t)(pumpPercentFiltered + 0.5f);
  setPumpLevel(powerPercentToDimmerLevel(pumpPercent));
}

void loop() {
  wdt_reset();
  const unsigned long now = millis();

  // 1. Нагрів — завжди вимкнено (страховка від будь-якого запису в D4)
  digitalWrite(HEATER_PIN, LOW);

  // 2. Потенціометр → диммер
  updatePumpFromPot(now);

  // 3. Термопара термоблоку
  updateTemperature(now);

  // 4. Вивід
  updateOled(now);
  logSerial(now);
}

// Debug Repeater — діагностичний скетч для DeLonghi EC685
//
// Мета (див. debug-repeater.md):
//   1. Зчитувати сигнал на оптопару SSR термоблока (штатний SDD)
//   2. Повторювати його на штатне SSR (passthrough), щоб кавоварка працювала як завжди
//   3. Логувати імпульси лічильника води
//   4. Логувати температуру з термопари MAX6675
//
// Підключення: Arduino Nano ↔ ПК по USB (Serial 115200).
// Формат логу (CSV) зручний для Serial Plotter / Excel / Python:
//   t_ms,sdd,heater,flow_pulses,flow_hz,temp_c,btn1,btn2,btn3,steam
//
// Бібліотеки (Менеджер бібліотек Arduino IDE):
//   1. Adafruit GFX Library
//   2. Adafruit SSD1306
//   3. max6675 (by Adafruit)

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <max6675.h>

// --- OLED 128x32 ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- MAX6675 ---
#define THERMO_DO  12
#define THERMO_CS  11
#define THERMO_CLK 13
MAX6675 thermocouple(THERMO_CLK, THERMO_CS, THERMO_DO);

// --- Піни (див. readme.md) ---
#define HEATER_PIN       4  // вихід на оптопару SSR термоблока
#define FLOWMETER_PIN    5  // імпульси лічильника води
#define SDD_PIN          6  // вхід штатного сигналу SDD (через оптопару)
#define BTN_1CUP_PIN     7
#define BTN_2CUP_PIN     8
#define BTN_STEAM_PIN    9
#define STEAM_HANDLE_PIN 10

// --- Періоди ---
const unsigned long LOG_INTERVAL_MS = 20;    // частота рядків у Serial (~50 Гц)
const unsigned long TEMP_INTERVAL_MS = 250;  // MAX6675 потребує ≥220 мс на конверсію
const unsigned long OLED_INTERVAL_MS = 200;
const unsigned long FLOW_WINDOW_MS = 200;    // вікно для розрахунку частоти імпульсів

// INPUT_PULLUP + оптопара на GND → активний сигнал = LOW
const bool ACTIVE_LOW = true;

volatile unsigned long flowPulsesTotal = 0;
volatile unsigned long flowPulsesWindow = 0;

unsigned long lastLogMs = 0;
unsigned long lastTempMs = 0;
unsigned long lastOledMs = 0;
unsigned long lastFlowCalcMs = 0;

float currentTemp = NAN;
float flowHz = 0.0;

uint8_t lastSdd = 0;
uint8_t lastHeater = 0;

bool isActive(uint8_t pin) {
  const int raw = digitalRead(pin);
  return ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
}

// Pin Change Interrupt на D5 (PCINT21 / PCMSK2 bit 5).
// На Nano зовнішні INT лише на D2/D3; D2 зайнятий диммером у фінальній прошивці.
ISR(PCINT2_vect) {
  static uint8_t lastState = HIGH;
  const uint8_t state = digitalRead(FLOWMETER_PIN);
  // Рахуємо зростаючий фронт (як у 001.cpp); за потреби змініть на FALLING
  if (lastState == LOW && state == HIGH) {
    flowPulsesTotal++;
    flowPulsesWindow++;
  }
  lastState = state;
}

void setupFlowmeterInterrupt() {
  pinMode(FLOWMETER_PIN, INPUT_PULLUP);
  // увімкнути PCINT для PORTD (D0–D7), лише біт D5
  PCICR |= (1 << PCIE2);
  PCMSK2 |= (1 << PCINT21);
}

void updateFlowRate(unsigned long now) {
  if (now - lastFlowCalcMs < FLOW_WINDOW_MS) {
    return;
  }

  noInterrupts();
  const unsigned long pulses = flowPulsesWindow;
  flowPulsesWindow = 0;
  interrupts();

  const float dtSec = (now - lastFlowCalcMs) / 1000.0;
  flowHz = (dtSec > 0.0) ? (pulses / dtSec) : 0.0;
  lastFlowCalcMs = now;
}

void passThroughHeater() {
  // Штатний SDD активний (оптопара тягне пін вниз) → вмикаємо наш вихід на SSR
  const bool sddOn = isActive(SDD_PIN);
  digitalWrite(HEATER_PIN, sddOn ? HIGH : LOW);
  lastSdd = sddOn ? 1 : 0;
  lastHeater = sddOn ? 1 : 0;
}

void logCsvLine(unsigned long now) {
  noInterrupts();
  const unsigned long pulses = flowPulsesTotal;
  interrupts();

  // t_ms,sdd,heater,flow_pulses,flow_hz,temp_c,btn1,btn2,btn3,steam
  Serial.print(now);
  Serial.print(',');
  Serial.print(lastSdd);
  Serial.print(',');
  Serial.print(lastHeater);
  Serial.print(',');
  Serial.print(pulses);
  Serial.print(',');
  Serial.print(flowHz, 2);
  Serial.print(',');
  if (isnan(currentTemp)) {
    Serial.print(F("nan"));
  } else {
    Serial.print(currentTemp, 2);
  }
  Serial.print(',');
  Serial.print(isActive(BTN_1CUP_PIN) ? 1 : 0);
  Serial.print(',');
  Serial.print(isActive(BTN_2CUP_PIN) ? 1 : 0);
  Serial.print(',');
  Serial.print(isActive(BTN_STEAM_PIN) ? 1 : 0);
  Serial.print(',');
  Serial.println(isActive(STEAM_HANDLE_PIN) ? 1 : 0);
}

void updateOled(unsigned long now) {
  if (now - lastOledMs < OLED_INTERVAL_MS) {
    return;
  }
  lastOledMs = now;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(F("SDD:"));
  display.print(lastSdd ? F("ON ") : F("off"));
  display.print(F(" FL:"));
  display.print(flowHz, 1);
  display.print(F("Hz"));

  display.setCursor(0, 12);
  display.print(F("T:"));
  if (isnan(currentTemp)) {
    display.print(F("ERR"));
  } else {
    display.print(currentTemp, 1);
    display.print(F("C"));
  }

  noInterrupts();
  const unsigned long pulses = flowPulsesTotal;
  interrupts();
  display.setCursor(64, 12);
  display.print(F("N:"));
  display.print(pulses);

  display.setCursor(0, 24);
  display.print(F("B:"));
  display.print(isActive(BTN_1CUP_PIN) ? '1' : '-');
  display.print(isActive(BTN_2CUP_PIN) ? '2' : '-');
  display.print(isActive(BTN_STEAM_PIN) ? 'S' : '-');
  display.print(isActive(STEAM_HANDLE_PIN) ? 'H' : '-');
  display.print(F(" LOG"));

  display.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(SDD_PIN, INPUT_PULLUP);
  pinMode(BTN_1CUP_PIN, INPUT_PULLUP);
  pinMode(BTN_2CUP_PIN, INPUT_PULLUP);
  pinMode(BTN_STEAM_PIN, INPUT_PULLUP);
  pinMode(STEAM_HANDLE_PIN, INPUT_PULLUP);

  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  setupFlowmeterInterrupt();

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println(F("# OLED init failed"));
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 12);
    display.print(F("Debug Repeater"));
    display.display();
  }

  delay(500); // перша конверсія MAX6675

  // Заголовок CSV (рядки з # ігноруються більшістю парсерів)
  Serial.println(F("# EC685 debug-repeater"));
  Serial.println(F("# t_ms,sdd,heater,flow_pulses,flow_hz,temp_c,btn1,btn2,btn3,steam"));

  const unsigned long now = millis();
  lastLogMs = now;
  lastTempMs = now;
  lastOledMs = now;
  lastFlowCalcMs = now;
}

void loop() {
  const unsigned long now = millis();

  // 1. Passthrough SDD → SSR (максимально часто)
  passThroughHeater();

  // 2. Частота імпульсів флоуметра
  updateFlowRate(now);

  // 3. Температура (рідко — обмеження MAX6675)
  if (now - lastTempMs >= TEMP_INTERVAL_MS) {
    lastTempMs = now;
    currentTemp = thermocouple.readCelsius();
  }

  // 4. CSV-лог у Serial
  if (now - lastLogMs >= LOG_INTERVAL_MS) {
    lastLogMs = now;
    logCsvLine(now);
  }

  // 5. Статус на OLED
  updateOled(now);
}

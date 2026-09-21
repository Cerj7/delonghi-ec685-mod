// Pump Dimmer + OLED — тест регулювання помпи та виводу на дисплей
//
// Мета (крок 8 у readme.md):
//   - зчитувати потенціометр потужності помпи (A0)
//   - плавно керувати помпою через AC-диммер (Zero-Cross D2, GATE D3)
//   - рахувати імпульси штатного лічильника води (D5, PCINT)
//   - виводити на OLED фіксований макет рядків для перевірки дисплея
//
// На OLED (тест макета, setTextSize(2)):
//   T = 95 C             ← фіксовано
//   P = n%               ← реальне значення з потенціометра
//
// Реальна частота імпульсів флоуметра пишеться в Serial (для калібрування).
// Піни збігаються з фінальним pinout у readme.md — без перепаювання.
//
// Бібліотеки (Arduino IDE):
//   1. Adafruit GFX Library
//   2. Adafruit SSD1306
//   3. AC_Dimmer (zip з https://github.com/serhiy23412/AC_dimmer-dmr-0.2.1)
//
// Обмеження AC_Dimmer:
//   - Zero-Cross завжди на D2
//   - D9/D10 не використовувати для AnalogWrite()

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <AC_Dimmer.h>

// --- OLED 0.91" 128x32 I2C (SDA=A4, SCL=A5) ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define OLED_I2C_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Піни (див. readme.md) ---
// Диммер: Zero-Cross жорстко D2 у бібліотеці; GATE — D3
#define DIMMER_GATE_PIN  3
#define HEATER_PIN       4   // зарезервовано (у цьому скетчі не керуємо)
#define FLOWMETER_PIN    5
#define SDD_PIN          6   // зарезервовано
#define BTN_1CUP_PIN     7   // зарезервовано
#define BTN_2CUP_PIN     8   // зарезервовано
#define BTN_STEAM_PIN    9   // зарезервовано (не PWM!)
#define STEAM_HANDLE_PIN 10  // зарезервовано (не PWM!)
// D11–D13 зарезервовані під MAX6675 у фінальній прошивці

#define PUMP_POT_PIN A0
// A1 зарезервовано під потенціометр температури

#define PUMP_DIMMER 0  // номер каналу диммера в AC_Dimmer (від 0)

// Фіксоване значення для тесту макета дисплея
const float DISPLAY_TARGET_TEMP_C = 95.0;

const unsigned long OLED_INTERVAL_MS = 150;
const unsigned long FLOW_WINDOW_MS = 200;
const unsigned long SERIAL_INTERVAL_MS = 200;

volatile unsigned long flowPulsesTotal = 0;
volatile unsigned long flowPulsesWindow = 0;

unsigned long lastOledMs = 0;
unsigned long lastFlowCalcMs = 0;
unsigned long lastSerialMs = 0;

int pumpPowerPercent = 0;  // 0–100
float flowHz = 0.0;

// Pin Change Interrupt на D5 (PCINT21).
// На Nano зовнішні INT лише D2/D3; D2 зайнятий Zero-Cross диммера.
ISR(PCINT2_vect) {
  static uint8_t lastState = HIGH;
  const uint8_t state = digitalRead(FLOWMETER_PIN);
  if (lastState == LOW && state == HIGH) {
    flowPulsesTotal++;
    flowPulsesWindow++;
  }
  lastState = state;
}

void setupFlowmeterInterrupt() {
  pinMode(FLOWMETER_PIN, INPUT_PULLUP);
  PCICR |= (1 << PCIE2);
  PCMSK2 |= (1 << PCINT21);
}

void setupReservedPins() {
  // Входи штатної плати — як у фінальній прошивці (оптопара → GND)
  pinMode(SDD_PIN, INPUT_PULLUP);
  pinMode(BTN_1CUP_PIN, INPUT_PULLUP);
  pinMode(BTN_2CUP_PIN, INPUT_PULLUP);
  pinMode(BTN_STEAM_PIN, INPUT_PULLUP);
  pinMode(STEAM_HANDLE_PIN, INPUT_PULLUP);

  // ТЕН вимкнений на час тесту помпи
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);
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

void updatePumpFromPot() {
  // A0: 0…1023 → 0…100%
  const int raw = analogRead(PUMP_POT_PIN);
  pumpPowerPercent = map(raw, 0, 1023, 0, 100);

  // AC_Dimmer::Dimm_value очікує 0…255
  const uint8_t dimmerLevel = (uint8_t)map(pumpPowerPercent, 0, 100, 0, 255);
  Dimm_value(PUMP_DIMMER, dimmerLevel);
}

void updateOled(unsigned long now) {
  if (now - lastOledMs < OLED_INTERVAL_MS) {
    return;
  }
  lastOledMs = now;

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);

  // Рядок 1 — фіксована цільова температура (тест макета)
  display.setCursor(0, 0);
  display.print(F("T = "));
  display.print(DISPLAY_TARGET_TEMP_C, 0);
  display.print(F(" C"));

  // Рядок 2 — реальна потужність помпи
  display.setCursor(0, 16);
  display.print(F("P = "));
  display.print(pumpPowerPercent);
  display.print(F("%"));

  display.display();
}

void logSerial(unsigned long now) {
  if (now - lastSerialMs < SERIAL_INTERVAL_MS) {
    return;
  }
  lastSerialMs = now;

  noInterrupts();
  const unsigned long pulses = flowPulsesTotal;
  interrupts();

  // t_ms,pump_pct,dimmer_0_255,flow_pulses,flow_hz
  Serial.print(now);
  Serial.print(',');
  Serial.print(pumpPowerPercent);
  Serial.print(',');
  Serial.print(map(pumpPowerPercent, 0, 100, 0, 255));
  Serial.print(',');
  Serial.print(pulses);
  Serial.print(',');
  Serial.println(flowHz, 2);
}

void setup() {
  Serial.begin(115200);

  setupReservedPins();
  setupFlowmeterInterrupt();

  // Ініціалізація диммера: Zero-Cross = D2 (всередині бібліотеки), GATE = D3
  Dimmer_init_begin();
  Dimmer_pin_assign(PUMP_DIMMER, DIMMER_GATE_PIN);
  Dimmer_init_end();
  Dimm_value(PUMP_DIMMER, 0);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Serial.println(F("# OLED init failed"));
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(2);
    display.setCursor(8, 12);
    display.print(F("Pump Dimmer Ready"));
    display.display();
    delay(800);
  }

  Serial.println(F("# EC685 pump-dimmer-display"));
  Serial.println(F("# t_ms,pump_pct,dimmer_0_255,flow_pulses,flow_hz"));

  const unsigned long now = millis();
  lastOledMs = now;
  lastFlowCalcMs = now;
  lastSerialMs = now;
}

void loop() {
  const unsigned long now = millis();

  updatePumpFromPot();
  updateFlowRate(now);
  updateOled(now);
  logSerial(now);
}

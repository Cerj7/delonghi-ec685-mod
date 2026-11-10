// Debug Repeater — діагностичний скетч для DeLonghi EC685
//
// Мета (див. debug-repeater.md / docs/debug-repeater.md):
//   1. Зчитувати штатний SDD термоблока і повторювати на SSR (passthrough)
//   2. Зчитувати штатний сигнал помпи (A3) і повторювати через AC-диммер (D3)
//   3. Логувати імпульси лічильника води та температуру MAX6675
//   4. У реальному часі показувати натискання кнопок і відкриття ручки пари
//
// Підключення: Arduino Nano ↔ ПК по USB (Serial 115200).
// Формат CSV:
//   t_ms,sdd,heater,pump_in,pump_out,flow_pulses,flow_hz,temp_c,btn1,btn2,btn3,steam
// Події кнопок/пари — окремі рядки `# EVT ...` одразу після зміни стану.
//
// Бібліотеки (Менеджер бібліотек Arduino IDE):
//   1. Adafruit GFX Library
//   2. Adafruit SSD1306
//   3. max6675 (by Adafruit)
//   4. AC_Dimmer (zip з https://github.com/serhiy23412/AC_dimmer-dmr-0.2.1)

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <max6675.h>
#include <AC_Dimmer.h>

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
// Диммер: Zero-Cross жорстко D2 у бібліотеці; GATE — D3
#define DIMMER_GATE_PIN  3
#define HEATER_PIN       4  // вихід на оптопару SSR термоблока
#define FLOWMETER_PIN    5  // імпульси лічильника води
#define SDD_PIN          6  // вхід штатного сигналу SDD (через оптопару)
#define BTN_1CUP_PIN     7
#define BTN_2CUP_PIN     8
#define BTN_STEAM_PIN    9
#define STEAM_HANDLE_PIN 10
#define PUMP_CTRL_PIN    A3 // вхід штатного сигналу помпи (через оптопару)

#define PUMP_DIMMER 0  // канал AC_Dimmer

// --- Періоди ---
const unsigned long LOG_INTERVAL_MS = 20;    // частота CSV (~50 Гц)
const unsigned long TEMP_INTERVAL_MS = 250;  // MAX6675 потребує ≥220 мс на конверсію
const unsigned long OLED_INTERVAL_MS = 100;  // швидше оновлення кнопок на екрані
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
uint8_t lastPumpIn = 0;
uint8_t lastPumpOut = 0;

uint8_t prevBtn1 = 0;
uint8_t prevBtn2 = 0;
uint8_t prevBtn3 = 0;
uint8_t prevSteam = 0;
bool btnStateInited = false;

bool isActive(uint8_t pin) {
  const int raw = digitalRead(pin);
  return ACTIVE_LOW ? (raw == LOW) : (raw == HIGH);
}

// Pin Change Interrupt на D5 (PCINT21 / PCMSK2 bit 5).
// На Nano зовнішні INT лише на D2/D3; D2 зайнятий Zero-Cross диммера.
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

void passThroughPump() {
  // Штатний сигнал помпи → 100% або 0% через диммер (повний passthrough ON/OFF)
  const bool pumpOn = isActive(PUMP_CTRL_PIN);
  Dimm_value(PUMP_DIMMER, pumpOn ? 255 : 0);
  lastPumpIn = pumpOn ? 1 : 0;
  lastPumpOut = pumpOn ? 1 : 0;
}

void printBtnState(uint8_t btn1, uint8_t btn2, uint8_t btn3, uint8_t steam) {
  Serial.print(F("1cup="));
  Serial.print(btn1 ? F("ON") : F("off"));
  Serial.print(F(" 2cup="));
  Serial.print(btn2 ? F("ON") : F("off"));
  Serial.print(F(" steam_btn="));
  Serial.print(btn3 ? F("ON") : F("off"));
  Serial.print(F(" steam_handle="));
  Serial.print(steam ? F("OPEN") : F("closed"));
}

void logControlEvents(unsigned long now) {
  const uint8_t btn1 = isActive(BTN_1CUP_PIN) ? 1 : 0;
  const uint8_t btn2 = isActive(BTN_2CUP_PIN) ? 1 : 0;
  const uint8_t btn3 = isActive(BTN_STEAM_PIN) ? 1 : 0;
  const uint8_t steam = isActive(STEAM_HANDLE_PIN) ? 1 : 0;

  if (!btnStateInited) {
    prevBtn1 = btn1;
    prevBtn2 = btn2;
    prevBtn3 = btn3;
    prevSteam = steam;
    btnStateInited = true;

    Serial.print(F("# EVT "));
    Serial.print(now);
    Serial.print(F(" INIT "));
    printBtnState(btn1, btn2, btn3, steam);
    Serial.println();
    return;
  }

  if (btn1 == prevBtn1 && btn2 == prevBtn2 && btn3 == prevBtn3 && steam == prevSteam) {
    return;
  }

  Serial.print(F("# EVT "));
  Serial.print(now);
  Serial.print(' ');

  if (btn1 != prevBtn1) {
    Serial.print(F("1CUP "));
    Serial.print(btn1 ? F("PRESS") : F("release"));
    Serial.print(' ');
  }
  if (btn2 != prevBtn2) {
    Serial.print(F("2CUP "));
    Serial.print(btn2 ? F("PRESS") : F("release"));
    Serial.print(' ');
  }
  if (btn3 != prevBtn3) {
    Serial.print(F("STEAM_BTN "));
    Serial.print(btn3 ? F("PRESS") : F("release"));
    Serial.print(' ');
  }
  if (steam != prevSteam) {
    Serial.print(F("STEAM_HANDLE "));
    Serial.print(steam ? F("OPEN") : F("CLOSE"));
    Serial.print(' ');
  }

  Serial.print(F("| "));
  printBtnState(btn1, btn2, btn3, steam);
  Serial.println();

  prevBtn1 = btn1;
  prevBtn2 = btn2;
  prevBtn3 = btn3;
  prevSteam = steam;
}

void logCsvLine(unsigned long now) {
  noInterrupts();
  const unsigned long pulses = flowPulsesTotal;
  interrupts();

  // t_ms,sdd,heater,pump_in,pump_out,flow_pulses,flow_hz,temp_c,btn1,btn2,btn3,steam
  Serial.print(now);
  Serial.print(',');
  Serial.print(lastSdd);
  Serial.print(',');
  Serial.print(lastHeater);
  Serial.print(',');
  Serial.print(lastPumpIn);
  Serial.print(',');
  Serial.print(lastPumpOut);
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

  const uint8_t btn1 = isActive(BTN_1CUP_PIN) ? 1 : 0;
  const uint8_t btn2 = isActive(BTN_2CUP_PIN) ? 1 : 0;
  const uint8_t btn3 = isActive(BTN_STEAM_PIN) ? 1 : 0;
  const uint8_t steam = isActive(STEAM_HANDLE_PIN) ? 1 : 0;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Рядок 1: passthrough ТЕН + помпа + flow
  display.setCursor(0, 0);
  display.print(F("H:"));
  display.print(lastHeater ? F("ON ") : F("off"));
  display.print(F(" P:"));
  display.print(lastPumpOut ? F("ON ") : F("off"));
  display.print(F("FL:"));
  display.print(flowHz, 1);

  // Рядок 2: температура + імпульси
  display.setCursor(0, 11);
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
  display.setCursor(64, 11);
  display.print(F("N:"));
  display.print(pulses);

  // Рядок 3: кнопки / ручка пари — головне для live-діагностики
  display.setCursor(0, 22);
  display.print(btn1 ? F("[1]") : F(" 1 "));
  display.print(btn2 ? F("[2]") : F(" 2 "));
  display.print(btn3 ? F("[S]") : F(" S "));
  display.print(steam ? F("[STEAM]") : F(" steam "));

  display.display();
}

void setup() {
  Serial.begin(115200);

  pinMode(SDD_PIN, INPUT_PULLUP);
  pinMode(PUMP_CTRL_PIN, INPUT_PULLUP);
  pinMode(BTN_1CUP_PIN, INPUT_PULLUP);
  pinMode(BTN_2CUP_PIN, INPUT_PULLUP);
  pinMode(BTN_STEAM_PIN, INPUT_PULLUP);
  pinMode(STEAM_HANDLE_PIN, INPUT_PULLUP);

  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  setupFlowmeterInterrupt();

  // Диммер: Zero-Cross = D2 (всередині бібліотеки), GATE = D3
  Dimmer_init_begin();
  Dimmer_pin_assign(PUMP_DIMMER, DIMMER_GATE_PIN);
  Dimmer_init_end();
  Dimm_value(PUMP_DIMMER, 0);

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

  Serial.println(F("# EC685 debug-repeater"));
  Serial.println(F("# t_ms,sdd,heater,pump_in,pump_out,flow_pulses,flow_hz,temp_c,btn1,btn2,btn3,steam"));
  Serial.println(F("# EVT lines: button / steam_handle edge events"));

  const unsigned long now = millis();
  lastLogMs = now;
  lastTempMs = now;
  lastOledMs = now;
  lastFlowCalcMs = now;
}

void loop() {
  const unsigned long now = millis();

  // 1. Passthrough SDD → SSR і штатна помпа → диммер (максимально часто)
  passThroughHeater();
  passThroughPump();

  // 2. Події кнопок / ручки пари — одразу в Serial
  logControlEvents(now);

  // 3. Частота імпульсів флоуметра
  updateFlowRate(now);

  // 4. Температура (рідко — обмеження MAX6675)
  if (now - lastTempMs >= TEMP_INTERVAL_MS) {
    lastTempMs = now;
    currentTemp = thermocouple.readCelsius();
  }

  // 5. CSV-лог у Serial
  if (now - lastLogMs >= LOG_INTERVAL_MS) {
    lastLogMs = now;
    logCsvLine(now);
  }

  // 6. Статус на OLED (кнопки виділені в нижньому рядку)
  updateOled(now);
}

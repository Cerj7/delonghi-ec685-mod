// Dual Thermocouple — режим вимірів тільки (крок 9 у readme.md)
//
// Мета:
//   - зчитувати обидві термопари MAX6675
//       Tt = термоблок / вихід (часто, для графіка)
//       Tw = вода в бачку (раз на 10 с)
//   - показувати Tt і Tw на OLED
//   - стримити в Serial лише Tt (графік ~1 хв на ПК)
//
// Графік НЕ малюємо на OLED: 128×32 замалий для хвилинної історії.
// Де будувати графік і як зберігати дані — scripts/004_plot_temps.py
//
// Підключення: Arduino Nano ↔ ПК по USB (Serial 115200).
// Піни збігаються з pinout у readme.md.
//
// Бібліотеки (Arduino IDE):
//   1. Adafruit GFX Library
//   2. Adafruit SSD1306
//   3. max6675 (by Adafruit)
//
// Формат Serial (рядки з # — коментарі, Serial Plotter їх ігнорує):
//   t_ms,Tt_C
//   # Tw,<t_ms>,<Tw_C>     ← лише коли оновилась вода (кожні 10 с)
//
// --- Де графік і зберігання ---
// 1) Arduino IDE → Tools → Serial Plotter
//    - порт 115200; одна крива Tt (рядки # Tw не потрапляють на графік)
// 2) scripts/004_plot_temps.py (рекомендовано)
//    - live-графік Tt, ковзне вікно 60 с; поточне Tw — підпис, не крива
//    - опційно CSV: logs/temps_YYYYMMDD_HHMMSS.csv

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <max6675.h>

// --- OLED 0.91" 128x32 I2C (SDA=A4, SCL=A5) ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define OLED_I2C_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Дві MAX6675: спільні SO/SCK, окремі CS ---
// Tt = термоблок / вихід, Tw = вода в бачку
#define THERMO_SO   12
#define THERMO_SCK  13
#define THERMO_CS_T 11   // Tt
#define THERMO_CS_W A2   // Tw

MAX6675 thermoT(THERMO_SCK, THERMO_CS_T, THERMO_SO);
MAX6675 thermoW(THERMO_SCK, THERMO_CS_W, THERMO_SO);

// --- Періоди ---
// MAX6675: ≥220 мс на конверсію після кожного read() цього ж чипа.
// Чипи незалежні (окремий CS), тому Tw раз на 10 с не збиває темп Tt.
const unsigned long TT_INTERVAL_MS = 250;
const unsigned long TW_INTERVAL_MS = 10000;
const unsigned long OLED_INTERVAL_MS = 200;
const unsigned long SERIAL_INTERVAL_MS = 250;

float ttC = NAN;  // термоблок
float twC = NAN;  // вода
bool twJustRead = false;

unsigned long lastTtMs = 0;
unsigned long lastTwMs = 0;
unsigned long lastOledMs = 0;
unsigned long lastSerialMs = 0;

void printTempOrNan(float value) {
  if (isnan(value)) {
    Serial.print(F("nan"));
  } else {
    Serial.print(value, 2);
  }
}

void updateOled(unsigned long now) {
  if (now - lastOledMs < OLED_INTERVAL_MS) {
    return;
  }
  lastOledMs = now;

  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);

  display.setCursor(0, 0);
  display.print(F("Tt="));
  if (isnan(ttC)) {
    display.print(F("---"));
  } else {
    display.print(ttC, 1);
  }
  display.print(F("C"));

  display.setCursor(0, 16);
  display.print(F("Tw="));
  if (isnan(twC)) {
    display.print(F("---"));
  } else {
    display.print(twC, 1);
  }
  display.print(F("C"));

  display.display();
}

void logSerial(unsigned long now) {
  if (twJustRead) {
    twJustRead = false;
    // Plotter ігнорує '#'; Python забирає Tw для підпису і CSV
    Serial.print(F("# Tw,"));
    Serial.print(now);
    Serial.print(',');
    printTempOrNan(twC);
    Serial.println();
  }

  if (now - lastSerialMs < SERIAL_INTERVAL_MS) {
    return;
  }
  lastSerialMs = now;

  // Лише Tt — єдина крива на графіку
  Serial.print(now);
  Serial.print(',');
  printTempOrNan(ttC);
  Serial.println();
}

void setup() {
  Serial.begin(115200);

  pinMode(THERMO_CS_T, OUTPUT);
  pinMode(THERMO_CS_W, OUTPUT);
  digitalWrite(THERMO_CS_T, HIGH);
  digitalWrite(THERMO_CS_W, HIGH);

  if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDR)) {
    Serial.println(F("# OLED init failed"));
  } else {
    display.clearDisplay();
    display.setTextColor(SSD1306_WHITE);
    display.setTextSize(1);
    display.setCursor(0, 12);
    display.print(F("Dual Thermo Ready"));
    display.display();
    delay(600);
  }

  // Перша конверсія MAX6675 після живлення
  delay(500);

  Serial.println(F("# EC685 dual-thermocouple (measure-only)"));
  Serial.println(F("# Tt=thermoblock (plot)  Tw=water every 10s (not plotted)"));
  Serial.println(F("# t_ms,Tt_C"));

  const unsigned long now = millis();
  lastTtMs = now;
  lastTwMs = now;
  lastOledMs = now;
  lastSerialMs = now;

  // Стартове Tw одразу, далі — кожні 10 с
  twC = thermoW.readCelsius();
  twJustRead = true;
}

void loop() {
  const unsigned long now = millis();

  if (now - lastTwMs >= TW_INTERVAL_MS) {
    lastTwMs = now;
    twC = thermoW.readCelsius();
    twJustRead = true;
  }

  if (now - lastTtMs >= TT_INTERVAL_MS) {
    lastTtMs = now;
    ttC = thermoT.readCelsius();
  }

  updateOled(now);
  logSerial(now);
}

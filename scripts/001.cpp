// Перед прошивкою встанови бібліотеки через Менеджер бібліотек Arduino:

// 1. `Adafruit GFX Library`
// 2. `Adafruit SSD1306`
// 3. `max6675` (by Adafruit)
// 4. `AC_dimmer-dmr-0.2.1` (із завантаженого zip)

#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <max6675.h>
#include <dimmer_ac.h> // Бібліотека serhiy23412

// --- Налаштування OLED Дисплея ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET    -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// --- Налаштування Термопари MAX6675 ---
#define THERMO_DO   12
#define THERMO_CS   11
#define THERMO_CLK  13
MAX6675 thermocouple(THERMO_CLK, THERMO_CS, THERMO_DO);

// --- Піни Диммера та Нагрівача ---
#define ZC_PIN       2  // Zero-Cross
#define DIMMER_PIN   3  // Gate помпи
#define HEATER_PIN   4  // Штатне реле ТЕНу (SSR)

// --- Вхідні сигнали (Опторозв'язка) ---
#define FLOWMETER_PIN    5
#define SDD_PIN          6
#define BTN_1CUP_PIN     7
#define BTN_2CUP_PIN     8
#define BTN_STEAM_PIN    9
#define STEAM_HANDLE_PIN 10

// --- Ініціалізація Диммера ---
ac_dimmer pumpDimmer(DIMMER_PIN, ZC_PIN);

// --- Глобальні змінні ---
volatile unsigned long flowPulses = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastHeaterUpdate = 0;
unsigned long lastFlowCalc = 0;

int targetPumpPower = 100; // %
float targetTemp = 92.0;   // °C
float currentTemp = 0.0;   // °C (виміряна термопарою на термоблоці)
float flowRate = 0.0;      // мл/с

// Вхідна температура води в бачку (константа)
const float T_water_in = 20.0; 

// Переривання флоуметра
void flowmeterISR() {
  flowPulses++;
}

void setup() {
  Serial.begin(115200);

  // Піни вхідних сигналів (підтяжка до VCC, спрацювання по GND через оптопару)
  pinMode(FLOWMETER_PIN, INPUT_PULLUP);
  pinMode(SDD_PIN, INPUT_PULLUP);
  pinMode(BTN_1CUP_PIN, INPUT_PULLUP);
  pinMode(BTN_2CUP_PIN, INPUT_PULLUP);
  pinMode(BTN_STEAM_PIN, INPUT_PULLUP);
  pinMode(STEAM_HANDLE_PIN, INPUT_PULLUP);

  // Керування ТЕНом
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  // Ініціалізація лічильника води
  attachInterrupt(digitalPinToInterrupt(FLOWMETER_PIN), flowmeterISR, RISING);

  // Ініціалізація диммера
  pumpDimmer.begin();

  // Ініціалізація OLED дисплея
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for (;;); // Зупинка при помилці I2C
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(10, 10);
  display.print(F("EC685 PID Ready"));
  display.display();
  delay(1000);
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Зчитування органів керування (Потенціометри)
  // A0: Потужність помпи (0 - 100%)
  int rawPump = analogRead(A0);
  targetPumpPower = map(rawPump, 0, 1023, 0, 100);

  // A1: Бажана температура води (80.0 - 98.0 °C)
  int rawTemp = analogRead(A1);
  targetTemp = map(rawTemp, 0, 1023, 800, 980) / 10.0;

  // Подача потужності на помпу
  pumpDimmer.setPower(targetPumpPower);

  // 2. Обчислення швидкості потоку (кожні 200 мс)
  if (currentMillis - lastFlowCalc >= 200) {
    noInterrupts();
    unsigned long pulses = flowPulses;
    flowPulses = 0;
    interrupts();

    // Калібрувальний коефіцієнт (зазвичай ~4.5 імпульси = 1 мл)
    flowRate = (pulses / 4.5) * (1000.0 / (currentMillis - lastFlowCalc)); 
    lastFlowCalc = currentMillis;
  }

  // 3. Зчитування температури термоблока та розрахунок нагріву (кожні 100 мс)
  if (currentMillis - lastHeaterUpdate >= 100) {
    lastHeaterUpdate = currentMillis;
    currentTemp = thermocouple.readCelsius();

    // Модель Feed-Forward: необхідна потужність залежно від потоку води
    // P_ff = Flow(мл/с) * 4.184 * (T_target - T_in)
    float p_feedforward = 0;
    if (flowRate > 0.3) {
      p_feedforward = flowRate * 4.184 * (targetTemp - T_water_in); 
    }

    // Гібридне керування ТЕНом:
    // Якщо температура термоблока нижча за цільову АБО є активна витрата води
    if (isnan(currentTemp)) {
      digitalWrite(HEATER_PIN, LOW); // Захист від обриву термопари
    } else if (currentTemp < targetTemp || p_feedforward > 150.0) {
      digitalWrite(HEATER_PIN, HIGH);
    } else {
      digitalWrite(HEATER_PIN, LOW);
    }
  }

  // 4. Оновлення дисплея (кожні 150 мс)
  if (currentMillis - lastDisplayUpdate >= 150) {
    lastDisplayUpdate = currentMillis;

    display.clearDisplay();
    
    // Рядок 1: Потужність помпи % та Поточна температура термоблока
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(F("PMP:"));
    display.print(targetPumpPower);
    display.print(F("%"));

    display.setCursor(64, 0);
    display.print(F("BLK:"));
    if (isnan(currentTemp)) {
      display.print(F("ERR"));
    } else {
      display.print(currentTemp, 1);
      display.print(F("C"));
    }

    // Рядок 2: Задана температура та Швидкість потоку
    display.setCursor(0, 16);
    display.print(F("SET:"));
    display.print(targetTemp, 1);
    display.print(F("C"));

    display.setCursor(64, 16);
    display.print(F("FLW:"));
    display.print(flowRate, 1);
    display.print(F("m/s"));

    display.display();
  }
}
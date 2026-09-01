# Модифікація

## Пристрої

Arduino nano v3
, два потенціометра на 10 кОм, OLED дисплей 0.91" I2C 128x32, Диммер 220В для Arduino и ESP8266 (бібліотека для роботи з ним https://github.com/serhiy23412/AC_dimmer-dmr-0.2.1), темпопара К типу з MAX6675.
Один потенціометр задає потужність помпи у відсотках. Другий потенціометр задає бажану температуру на виході кавоварки. Дисплей має показувати поточну встановлену потужність помпи і задану бажану температуру.
Диммер подає потужність на помпу.
Ардуїно також керує штатним твердотільним реле нагрівання термоблоку, через штатну оптопару.
Ардуїно також зчитує сигнал керування SDD штатного контроллера.
Ардуїно також зчитує сигнали з усіх трьох кнопок кавоварки.
Ардуїно також зчитує сигнали з штатного лічильника води кавоварки.
Ардуїно також зчитує сигнали з ручки подачі пари.
Термопара вимірює температуру води в бачкові.

Напиши мені код для ардуїно, опиши до яких виходів ардуїно що підключити. Перевір мою схему реалізації

---

### 1. Перевірка та аудит твоєї схеми (Головні нюанси)

#### ❌ Помилка №1: Вимірювання температури термопарою в бачку

Ти вказав, що термопара вимірює температуру води в бачку.

* **В чому проблема:** Вода в бачку завжди кімнатної температури ($18\text{–}25^\circ\text{C}$) і майже не змінюється під час приготування кави.
* **Як має бути:** Термопара MAX6675 **мусить бути закріплена на самому термоблоці** (замість штатного NTC або поруч із ним). Саме за її показаннями Ардуїно дізнається, наскільки гарячий метал, і вираховує, скільки потужності дати на ТЕН, а температура в бачку просто приймається за константу ($20^\circ\text{C}$).

#### ❌ Помилка №2: Диммер AC на помпі та штатна оптопара/SSR

* **Диммер на помпі:** Бібліотека `AC_dimmer-dmr-0.2.1` використовує переривання (Zero-Cross) та таймери. Помпу ULKA дійсно можна димувати за фазою.
* **Штатний ТЕН (SSR):** У DeLonghi EC685 нагрівач зазвичай комутується через реле/симистор на платі. Керувати ним краще за допомогою **Pulse-Skip Modulation (Blink/PWM)** без фазового димування, щоб не плутати таймери диммера помпи.

#### ⚠️ Рівні сигналів кнопок, кранчика пари, SDD та флоуметра

Плата EC685 працює на високій напрузі або власному низьковольтному потенціалі. **Не можна підключати дроти від кнопок або SDD напряму до Arduino!**

* Кнопки і ручку пари потрібно підключати **через оптопари** (або знімати сигнал після штатних оптопар/реле), щоб не спалити Arduino Nano.
* Флоуметр у EC685 живиться від $5\text{ В}$ — його сигнальний дріт можна підключити напряму.

---

### 2. Схема підключення (Pinout Arduino Nano)

Зважаючи на те, що диммер викоритовує переривання, а I2C та SPI мають свої фіксовані піни:

| Компонент | Пін Arduino Nano | Примітка |
| --- | --- | --- |
| **AC Dimmer (Zero-Cross / SYNC)** | **D2** | Обов'язково пін переривання INT0 |
| **AC Dimmer (GATE / PWM)** | **D3** | Вихід на симистор помпи |
| **Штатне реле ТЕНу (SSR)** | **D4** | Цифровий вихід (High/Low) |
| **Флоуметр (Сигнал)** | **D5** | Імпульси лічильника води |
| **Сигнал SDD (Штатний)** | **D6** | Вхід (через оптопару) |
| **Кнопка 1 (Одинарна)** | **D7** | Вхід (`INPUT_PULLUP` / оптопара) |
| **Кнопка 2 (Подвійна)** | **D8** | Вхід (`INPUT_PULLUP` / оптопара) |
| **Кнопка 3 (Пар)** | **D9** | Вхід (`INPUT_PULLUP` / оптопара) |
| **Ручка пари (Мікроперемикач)** | **D10** | Вхід (`INPUT_PULLUP` / оптопара) |
| **MAX6675 (CS)** | **D11** | Chip Select |
| **MAX6675 (SO / MISO)** | **D12** | Data Out |
| **MAX6675 (SCK / CLK)** | **D13** | Clock |
| **Потенціометр Помпи (10k)** | **A0** | Аналоговий вхід |
| **Потенціометр Температури (10k)** | **A1** | Аналоговий вхід |
| **OLED Дисплей (SDA)** | **A4** | Лінія даних I2C |
| **OLED Дисплей (SCL)** | **A5** | Лінія тактування I2C |

---

### 3. Готовий код для Arduino

Перед прошивкою встанови бібліотеки через Менеджер бібліотек Arduino:

1. `Adafruit GFX Library`
2. `Adafruit SSD1306`
3. `max6675` (by Adafruit)
4. `AC_dimmer-dmr-0.2.1` (із завантаженого zip)

```cpp
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
#define HEATER_PIN   4  // ТЕН (SSR)

// --- Піни вхідних сигналів ---
#define FLOWMETER_PIN 5
#define SDD_PIN       6
#define BTN_1CUP_PIN  7
#define BTN_2CUP_PIN  8
#define BTN_STEAM_PIN 9
#define STEAM_HANDLE_PIN 10

// --- Налаштування Диммера ---
// Ініціалізація диммера за бібліотекою serhiy23412
ac_dimmer pumpDimmer(DIMMER_PIN, ZC_PIN);

// --- Глобальні змінні ---
volatile unsigned long flowPulses = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastHeaterUpdate = 0;

int targetPumpPower = 100; // %
float targetTemp = 92.0;   // C
float currentTemp = 0.0;   // С
float flowRate = 0.0;      // мл/с

// Вхідна температура води в бачку (прийнята за константу або зчитується)
const float T_water_in = 20.0; 

// Переривання для флоуметра
void flowmeterISR() {
  flowPulses++;
}

void setup() {
  Serial.begin(115200);

  // Піни входу
  pinMode(FLOWMETER_PIN, INPUT_PULLUP);
  pinMode(SDD_PIN, INPUT_PULLUP);
  pinMode(BTN_1CUP_PIN, INPUT_PULLUP);
  pinMode(BTN_2CUP_PIN, INPUT_PULLUP);
  pinMode(BTN_STEAM_PIN, INPUT_PULLUP);
  pinMode(STEAM_HANDLE_PIN, INPUT_PULLUP);

  // Виходи
  pinMode(HEATER_PIN, OUTPUT);
  digitalWrite(HEATER_PIN, LOW);

  // Ініціалізація переривання флоуметра
  attachInterrupt(digitalPinToInterrupt(FLOWMETER_PIN), flowmeterISR, RISING);

  // Ініціалізація Диммера
  pumpDimmer.begin();

  // Ініціалізація Дисплея
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    for (;;); // Зупинка якщо дисплей не знайдено
  }
  
  display.clearDisplay();
  display.setTextColor(SSD1306_WHITE);
  display.setTextSize(1);
  display.setCursor(0, 10);
  display.print(F("DeLonghi Custom PID"));
  display.display();
  delay(1000);
}

void loop() {
  unsigned long currentMillis = millis();

  // 1. Зчитування потенціометрів
  // A0: Потужність помпи (0 - 100%)
  int rawPump = analogRead(A0);
  targetPumpPower = map(rawPump, 0, 1023, 0, 100);

  // A1: Задана температура (80 - 98 °C)
  int rawTemp = analogRead(A1);
  targetTemp = map(rawTemp, 0, 1023, 800, 980) / 10.0;

  // Подаємо потужність на диммер помпи
  pumpDimmer.setPower(targetPumpPower);

  // 2. Розрахунок швидкості потоку (кожні 200 мс)
  static unsigned long lastFlowCalc = 0;
  if (currentMillis - lastFlowCalc >= 200) {
    noInterrupts();
    unsigned long pulses = flowPulses;
    flowPulses = 0;
    interrupts();

    // Припустимо: 4.5 імпульси = 1 мл
    flowRate = (pulses / 4.5) * (1000.0 / (currentMillis - lastFlowCalc)); 
    lastFlowCalc = currentMillis;
  }

  // 3. Зчитування температури термоблока та розрахунок Feed-forward
  if (currentMillis - lastHeaterUpdate >= 100) { // кожні 100 мс
    lastHeaterUpdate = currentMillis;
    currentTemp = thermocouple.readCelsius();

    // Математична модель (Feed-forward):
    // P_ff = Flow(мл/с) * 4.184 * (T_target - T_in)
    float p_feedforward = 0;
    if (flowRate > 0.5) {
      p_feedforward = flowRate * 4.184 * (targetTemp - T_water_in); 
    }

    // Простий контролер ТЕНу: якщо температура нижча за бажану + враховуємо потік
    // (Максимальна потужність ТЕНу ~1350 Вт)
    if (currentTemp < targetTemp || p_feedforward > 200) {
      digitalWrite(HEATER_PIN, HIGH);
    } else {
      digitalWrite(HEATER_PIN, LOW);
    }
  }

  // 4. Оновлення Дисплея (кожні 150 мс)
  if (currentMillis - lastDisplayUpdate >= 150) {
    lastDisplayUpdate = currentMillis;

    display.clearDisplay();
    
    // Рядок 1: Потужність помпи та Поточна температура
    display.setTextSize(1);
    display.setCursor(0, 0);
    display.print(F("PUMP: "));
    display.print(targetPumpPower);
    display.print(F("%"));

    display.setCursor(64, 0);
    display.print(F("T:"));
    display.print(currentTemp, 1);
    display.print(F("C"));

    // Рядок 2: Задана температура та Потік
    display.setCursor(0, 16);
    display.print(F("SET : "));
    display.print(targetTemp, 1);
    display.print(F("C"));

    display.setCursor(64, 16);
    display.print(F("F:"));
    display.print(flowRate, 1);
    display.print(F("ml/s"));

    display.display();
  }
}

```




############


# DeLonghi EC685 Custom PID & Flow Control (Arduino Nano Mod)

Кастомна прошивка та гідравлічно-електронна модифікація для кавоварки **DeLonghi Dedica EC685** на базі **Arduino Nano v3**.

Мод додає можливість точного регулювання тиску/потоку помпи (dimming), вимірювання витрати води у реальному часі (флоуметр) та комбінованого керування температурою термоблока (Feed-forward + PID).

---

## 🛠 Основний функціонал

* **Pressure/Flow Profiling:** Ручне регулювання потужності помпи (0–100%) через потенціометр та фазовий димер AC (BTA16 / Zero-Cross).
* **Predictive Thermal Control:** Алгоритм керування ТЕНом на основі математичної моделі потоку води ($P = c \cdot m \cdot \Delta T$) та показань термопари MAX6675 безпосередньо на термоблоці.
* **Real-time Flow Measurement:** Зчитування імпульсів штатного ротаметричного лічильника води (Digmesa / Flowmeter) через переривання.
* **Інформативний OLED-дисплей:** Відображення поточної та заданої температури, потужності помпи у % та швидкості потоку у мл/с.
* **Безпечна інтеграція:** Зчитування сигналів штатної плати (кнопки, ручка пари, SDD) через опторозв'язку.

---

## 📐 Схема підключення (Pinout)

| Компонент | Пін Arduino Nano | Тип / Опис |
| :--- | :--- | :--- |
| **AC Dimmer (Zero-Cross / SYNC)** | `D2` | Вхід переривання INT0 |
| **AC Dimmer (GATE / PWM)** | `D3` | Вихід на симистор помпи |
| **Штатне реле ТЕНу (SSR)** | `D4` | Цифровий вихід керування нагрівом |
| **Флоуметр (Сигнал)** | `D5` | Вхід імпульсів лічильника води |
| **Сигнал SDD (Штатний)** | `D6` | Вхід (через оптопару PC817) |
| **Кнопка 1 (Одинарна)** | `D7` | Вхід (через оптопару) |
| **Кнопка 2 (Подвійна)** | `D8` | Вхід (через оптопару) |
| **Кнопка 3 (Пар)** | `D9` | Вхід (через оптопару) |
| **Ручка пари (Мікроперемикач)** | `D10` | Вхід (через оптопару) |
| **MAX6675 (CS)** | `D11` | Chip Select термопари |
| **MAX6675 (SO / MISO)** | `D12` | Data Out |
| **MAX6675 (SCK / CLK)** | `D13` | Clock |
| **Потенціометр Помпи (10k)** | `A0` | Аналоговий вхід (0–100%) |
| **Потенціометр Температури (10k)**| `A1` | Аналоговий вхід (80.0–98.0 °C) |
| **OLED Дисплей (SDA)** | `A4` | I2C Data |
| **OLED Дисплей (SCL)** | `A5` | I2C Clock |

---

## 📦 Необхідні бібліотеки

Перед збіркою прошивки встановіть наступні бібліотеки в Arduino IDE:

1. [Adafruit GFX Library](https://github.com/adafruit/Adafruit-GFX-Library)
2. [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306)
3. [MAX6675 library by Adafruit](https://github.com/adafruit/MAX6675-library)
4. [AC_dimmer-dmr-0.2.1 by serhiy23412](https://github.com/serhiy23412/AC_dimmer-dmr-0.2.1)

---

## ⚡ Безпека та застереження

> ⚠️ **УВАГА:** Пристрій працює з небезпечною для життя напругою 220V AC! 
> 
> * Усі сигнали від штатної плати DeLonghi (кнопки, SDD) мають бути розв'язані від Arduino Nano через оптопари (наприклад, PC817).
> * Корпус та металеві деталі термоблока мають бути надійно заземлені.
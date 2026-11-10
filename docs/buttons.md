Аналіз схемотехніки плати кнопок DeLonghi Dedica EC685 за вашою схемою:

Плата містить 3 кнопки та 4 світлодіоди (білі підсвічування/підкладки та, ймовірно, двоколірну індикацію пара/декальцинації). Схема побудована за принципом **матричного сканування з суміщеними лініями (мультиплексування)** для економії дротів шлейфу.

---

### Аналіз виводів (Pinout):

1. **Pin 1 (Зелений):** Анод LED3 та LED4.
2. **Pin 2 (Фіолетовий):** Анод LED1 та LED2, а також анод розв'язувального діода кнопки «1 Еспресо».
3. **Pin 3 (Сірий):** Спільний катод для LED1 та LED3.
4. **Pin 4 (Рожевий):** Спільний катод для LED2 та LED4 + спільний контакт для кнопок «2 Еспресо» та «Пар».
5. **Pin 5 (Темно-сірий):** Контакт кнопки «1 Еспресо».
6. **Pin 6 (Темно-сірий):** Контакт кнопки «2 Еспресо» та кнопки «Пар».

---

### Як це працює і як зчитувати кнопки:

* **Кнопка «1 Еспресо»:**
Підключена між **Pin 5** та анодом діода на **Pin 2**. При натисканні замикається ланцюг між Pin 5 та Pin 2.
* **Кнопка «2 Еспресо»:**
Замикає **Pin 6** на **Pin 5** (другий контакт кнопки 1 Еспресо).
* **Кнопка «Пар»:**
Замикає **Pin 6** на **Pin 4**.

Щоб коректно керувати цією матрицею без дрижання контактів та без хибного вмикання світлодіодів:

1. Кнопка **1 Еспресо** зчитується подачею `HIGH` на **Pin 2** (через діод) та зчитуванням сигналу на **Pin 5** (зі стягуючим резистором `INPUT_PULLDOWN` або за допомогою `INPUT_PULLUP` на Pin 2 / Pin 5). Найкращий стандартний спосіб для Arduino без зовнішніх резисторів:
* Встановлюємо **Pin 5** та **Pin 6** у режим `INPUT_PULLUP` (внутрішній підтягуючий резистор до 5V).
* Встановлюємо **Pin 4** у стан `OUTPUT`, LOW — це дозволяє зчитувати **кнопку «Пар»** через **Pin 6**.
* Для кнопки **1 Еспресо**: імпульсно подаємо `LOW` на **Pin 2** (OUTPUT LOW), при цьому діод відкривається, коли кнопка натиснута, і тягне **Pin 5** до `LOW`.
* Для кнопки **2 Еспресо**: коли **Pin 5** з'єднується з **Pin 6**, натискання замикає їх між собою.



---

### Схема підключення до Arduino Nano v3

| Пін плати кнопок DeLonghi | Функція у схемі | Пін Arduino Nano |
| --- | --- | --- |
| **Pin 1** (Зелений) | Анод LED3 / LED4 | **D7** |
| **Pin 2** (Фіолетовий) | Анод LED1 / LED2 + Діод кнопки 1 | **D8** |
| **Pin 3** (Сірий) | Катод LED1 / LED3 | **D9** |
| **Pin 4** (Рожевий) | Катод LED2 / LED4 + Спільний кнопок 2 і Пар | **D10** |
| **Pin 5** (Темно-сірий) | Сигнал кнопки 1 Еспресо | **D4** |
| **Pin 6** (Темно-сірий) | Сигнал кнопок 2 Еспресо та Пар | **D12** |

> **Примітка:** Світлодіоди керуються матричним способом. Щоб подати струм на світлодіод, відповідний анод (Pin 1 або Pin 2) переводиться в `HIGH`, а катод (Pin 3 або Pin 4) в `LOW`. Для запобігання перевантаженню виходів Arduino рекомендується встановити послідовні резистори 220–330 Ом на аноди (Pin 1 та Pin 2).

---

---

### Схема підключення до Arduino Nano v3, що керує кнопками, до основної Arduino Nano v3
| Пін Arduino, що керує кнопками | Пін основної Arduino |
| --- | --- |
| `D6` | `D9` |
| `D5` | `D8` |

**Робочий UI панелі** (індикація за командами основної, події `B1`/`B2`/`B3`):

| Документ / скетч | Призначення |
| --- | --- |
| [`docs/button-panel-protocol.md`](button-panel-protocol.md) | Контракт UART: коди `HE1`…`DC0`, пріоритети LED4 |
| [`scripts/011_button_panel_ui.cpp`](../scripts/011_button_panel_ui.cpp) | Прошивка Nano на панелі кнопок |
| [`scripts/012_main_panel_link.cpp`](../scripts/012_main_panel_link.cpp) | Заглушка основної Nano (прийом/передача) |
| [`docs/button-panel-ui.md`](button-panel-ui.md) | Опис поведінки `011` і стенда |

Тестери матриці (`007`…`010`) — окремо, див. [`button-panel.md`](button-panel.md).

---

### Прошивка для Arduino Nano v3

> ⚠️ **Код нижче — чернетка (застарілий).** Піни в ньому (`D3`…`D8`) не збігаються з таблицею вище (`D7`…`D12`), немає антибрязкоту, немає протоколу до основної Arduino і немає гарантії, що LED не світяться під час сканування кнопок. Робоча версія — [`scripts/009_button_panel.cpp`](../scripts/009_button_panel.cpp), опис — [`docs/button-panel.md`](button-panel.md).

> ⚠️ **Головне: таблиця «Pin5 ↔ Pin6» і «Pin4 ↔ Pin6» вище — неправильна.** Вона дає кнопкам спільну лінію `Pin6`, якої в схемі [`assets/buttons-scheme.png`](../assets/buttons-scheme.png) у кнопках немає. Реальні пари визначаються на живій платі командою `D` (див. док).

Нижче наведено повністю робочий код з мультиплексуванням LED та скануванням кнопок без затримок (`delay` не використовується, щоб світлодіоди не мерехтіли):

```cpp
// Конфігурація пінів Arduino
const uint8_t PIN_BOARD_1 = 3; // Pin 1: Anode LED3, LED4
const uint8_t PIN_BOARD_2 = 4; // Pin 2: Anode LED1, LED2 + Button 1 Diode
const uint8_t PIN_BOARD_3 = 5; // Pin 3: Cathode LED1, LED3
const uint8_t PIN_BOARD_4 = 6; // Pin 4: Cathode LED2, LED4 + Common Btn 2/Steam
const uint8_t PIN_BOARD_5 = 7; // Pin 5: Button 1 Espresso (зафіксовано D7)
const uint8_t PIN_BOARD_6 = 8; // Pin 6: Button 2 Espresso & Steam (зафіксовано D8)

// Стан світлодіодів (true - увімкнено, false - вимкнено)
bool led1_state = true;  // 1 Еспресо підсвічування
bool led2_state = false; // 2 Еспресо підсвічування
bool led3_state = false; // Пар підсвічування
bool led4_state = false; // Додаткова індикація / Пара 2

// Стан кнопок
bool btn1_pressed = false; // 1 Еспресо
bool btn2_pressed = false; // 2 Еспресо
bool btnSteam_pressed = false; // Пар

// Таймери для неблокуючого виконання
unsigned long lastScanTime = 0;
const unsigned long SCAN_INTERVAL = 5; // Мультиплексування кожні 5 мс

void setup() {
  Serial.begin(115200);

  // Налаштування виводів за замовчуванням у високий опір
  pinMode(PIN_BOARD_1, INPUT);
  pinMode(PIN_BOARD_2, INPUT);
  pinMode(PIN_BOARD_3, INPUT);
  pinMode(PIN_BOARD_4, INPUT);
  
  // Внутрішня підтяжка для ліній зчитування кнопок
  pinMode(PIN_BOARD_5, INPUT_PULLUP);
  pinMode(PIN_BOARD_6, INPUT_PULLUP);
}

void loop() {
  unsigned long currentMillis = millis();

  // Цикл оновлення індикації та зчитування кнопок
  if (currentMillis - lastScanTime >= SCAN_INTERVAL) {
    lastScanTime = currentMillis;
    
    scanButtons();
    updateLEDs();
  }
}

// Функція зчитування стану кнопок
void scanButtons() {
  // Вимикаємо всі керуючі лінії LED перед скануванням
  pinMode(PIN_BOARD_1, INPUT);
  pinMode(PIN_BOARD_2, INPUT);
  pinMode(PIN_BOARD_3, INPUT);
  
  // 1. Сканування кнопки "Пар" (Pin 6 -> Pin 4)
  pinMode(PIN_BOARD_4, OUTPUT);
  digitalWrite(PIN_BOARD_4, LOW);
  pinMode(PIN_BOARD_6, INPUT_PULLUP);
  delayMicroseconds(20);
  btnSteam_pressed = (digitalRead(PIN_BOARD_6) == LOW);

  // 2. Сканування кнопки "1 Еспресо" (Pin 5 -> Pin 2 через діод)
  pinMode(PIN_BOARD_4, INPUT);
  pinMode(PIN_BOARD_2, OUTPUT);
  digitalWrite(PIN_BOARD_2, LOW);
  pinMode(PIN_BOARD_5, INPUT_PULLUP);
  delayMicroseconds(20);
  bool rawBtn1 = (digitalRead(PIN_BOARD_5) == LOW);

  // 3. Сканування кнопки "2 Еспресо" (Pin 6 -> Pin 5)
  pinMode(PIN_BOARD_2, INPUT);
  pinMode(PIN_BOARD_5, OUTPUT);
  digitalWrite(PIN_BOARD_5, LOW);
  pinMode(PIN_BOARD_6, INPUT_PULLUP);
  delayMicroseconds(20);
  bool rawBtn2 = (digitalRead(PIN_BOARD_6) == LOW);
  
  // Скидаємо Pin 5 у вихідний стан
  pinMode(PIN_BOARD_5, INPUT_PULLUP);

  // Логіка визначення натискань з розділенням перехресних зв'язків
  btn1_pressed = rawBtn1 && !rawBtn2;
  btn2_pressed = rawBtn2;

  // Вивід стану в Serial Monitor при зміні
  static bool p1 = false, p2 = false, pS = false;
  if (btn1_pressed != p1 || btn2_pressed != p2 || btnSteam_pressed != pS) {
    p1 = btn1_pressed;
    p2 = btn2_pressed;
    pS = btnSteam_pressed;
    
    Serial.print("Кнопки [ 1 Еспресо: ");
    Serial.print(btn1_pressed ? "НАТИСНУТО" : "---");
    Serial.print(" | 2 Еспресо: ");
    Serial.print(btn2_pressed ? "НАТИСНУТО" : "---");
    Serial.print(" | Пар: ");
    Serial.print(btnSteam_pressed ? "НАТИСНУТО" : "---");
    Serial.println(" ]");

    // Приклад реакції: перемикання підсвічування при натисканні
    if (btn1_pressed) { led1_state = true; led2_state = false; led3_state = false; }
    if (btn2_pressed) { led1_state = false; led2_state = true; led3_state = false; }
    if (btnSteam_pressed) { led1_state = false; led2_state = false; led3_state = true; }
  }
}

// Функція динамічної індикації LED (мультиплексування)
void updateLEDs() {
  static uint8_t step = 0;

  // Скидаємо всі піни в Z-стан (вхід)
  pinMode(PIN_BOARD_1, INPUT);
  pinMode(PIN_BOARD_2, INPUT);
  pinMode(PIN_BOARD_3, INPUT);
  pinMode(PIN_BOARD_4, INPUT);

  switch (step) {
    case 0: // LED 1 (Pin 2 Anode -> Pin 3 Cathode)
      if (led1_state) {
        pinMode(PIN_BOARD_2, OUTPUT); digitalWrite(PIN_BOARD_2, HIGH);
        pinMode(PIN_BOARD_3, OUTPUT); digitalWrite(PIN_BOARD_3, LOW);
      }
      break;
    case 1: // LED 2 (Pin 2 Anode -> Pin 4 Cathode)
      if (led2_state) {
        pinMode(PIN_BOARD_2, OUTPUT); digitalWrite(PIN_BOARD_2, HIGH);
        pinMode(PIN_BOARD_4, OUTPUT); digitalWrite(PIN_BOARD_4, LOW);
      }
      break;
    case 2: // LED 3 (Pin 1 Anode -> Pin 3 Cathode)
      if (led3_state) {
        pinMode(PIN_BOARD_1, OUTPUT); digitalWrite(PIN_BOARD_1, HIGH);
        pinMode(PIN_BOARD_3, OUTPUT); digitalWrite(PIN_BOARD_3, LOW);
      }
      break;
    case 3: // LED 4 (Pin 1 Anode -> Pin 4 Cathode)
      if (led4_state) {
        pinMode(PIN_BOARD_1, OUTPUT); digitalWrite(PIN_BOARD_1, HIGH);
        pinMode(PIN_BOARD_4, OUTPUT); digitalWrite(PIN_BOARD_4, LOW);
      }
      break;
  }

  step = (step + 1) % 4;
}

```
// Button Panel Tester — 007_button_panel.cpp
//
// Мета:
//   1. Керувати штатною платою кнопок DeLonghi EC685 (3 кнопки + 4 LED):
//        Pin1 → D7   аноди LED3 / LED4
//        Pin2 → D8   аноди LED1 / LED2 + спільний вузол кнопок (через діод)
//        Pin3 → D9   катоди LED1 / LED3
//        Pin4 → D10  катоди LED2 / LED4 + приймач кнопки «пар»
//        Pin5 → D4   приймач кнопки «1 еспресо»
//        Pin6 → D12  приймач «2 еспресо» / джерело для «пар»
//   2. Режим спокою: світлодіоди мигають по черзі — LED1 → LED2 → LED3 → LED4.
//   3. Натиснули кнопку → горить ЛИШЕ її світлодіод 2 с, решта гасне,
//      далі чергування продовжується з наступного LED.
//   4. Кожне натискання/відпускання летить у два канали:
//        • USB → ПК, 115200 — людський текст для Serial Monitor;
//        • SoftwareSerial D6 → основна Arduino Nano (D9), 19200 — компактно:
//          B1P / B1R / B2P / B2R / BSP / BSR + '\n'.
//   5. Команди з ПК: L1…L4 (примусово LED), T (прогін), A (авто),
//      M (розводка), X<кнопка><LED> (перепризначити LED кнопці), ? (довідка).
//
// Чому лінії розділені в часі:
//   Кнопки й LED сидять на одних і тих самих лініях (матриця 2×2 + 3 кнопки).
//   Тому сканування кнопок завжди починається з переведення ВСІХ ліній плати
//   в hi-Z — інакше натиснута кнопка «засвітить» чужий LED, а ввімкнений LED
//   зіпсує зчитування кнопки. LED на цей час (~0.2 мс кожні 5 мс) гасне —
//   оком не видно.
//
// Як читаються кнопки (метод із 010_simple_button_test.cpp):
//   Спільний вузол — Pin2 (D8). Діод проводить «від Pin2», тому тягнути лінію
//   вниз марно. Потрібно подавати HIGH на джерело і читати HIGH на приймачі.
//   D4 і D10 стягнуті зовнішніми резисторами до GND.
//     Такт 1: D8 = HIGH → D4 = «1 еспресо», D12 = «2 еспресо»
//             (btn1 = raw1 && !raw2 — щоб не плутати з кнопкою 2)
//     Такт 2: D12 = HIGH → D10 = «пар»
//
// Бібліотеки: SoftwareSerial входить у пакет AVR core — ставити нічого не треба.
//
// Опис, протокол і застереження — docs/button-panel.md

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <avr/wdt.h>
#include <ctype.h>

// ─────────────────────────── Налаштування ───────────────────────────

#define USE_WATCHDOG 1            // 2 с: зависання → reset → лінії в hi-Z, LED гаснуть

const uint8_t PIN_BOARD_1 = 7;    // аноди LED3 / LED4
const uint8_t PIN_BOARD_2 = 8;    // аноди LED1 / LED2 + спільний вузол кнопок
const uint8_t PIN_BOARD_3 = 9;    // катоди LED1 / LED3
const uint8_t PIN_BOARD_4 = 10;   // катоди LED2 / LED4 + приймач «пар» (підтяжка до GND)
const uint8_t PIN_BOARD_5 = 4;    // приймач «1 еспресо» (підтяжка до GND)
const uint8_t PIN_BOARD_6 = 12;   // приймач «2 еспресо» / джерело для «пар»

const uint8_t LINK_RX_PIN = 5;    // зарезервовано (D5 ↔ D8 основної), не читаємо
const uint8_t LINK_TX_PIN = 6;    // D6 → D9 основної
const long    LINK_BAUD   = 19200;
const long    PC_BAUD     = 115200;

const uint16_t CHASE_STEP_MS  = 150;   // крок «бігучого вогника» у спокої
const uint16_t SOLID_HOLD_MS  = 2000;  // скільки горить LED натиснутої кнопки
const uint16_t MANUAL_HOLD_MS = 5000;  // примусове L1…L4
const uint16_t WALK_STEP_MS   = 700;   // крок прогону T
const uint16_t SCAN_INTERVAL_MS = 5;   // період сканування кнопок
const uint16_t SETTLE_US      = 20;    // пауза на стабілізацію через діоди
const uint8_t  DEBOUNCE_SAMPLES = 3;   // 3 × 5 мс = 15 мс антибрязкіт

// ─────────────────────────── Таблиці ───────────────────────────

enum LedId { LED_1, LED_2, LED_3, LED_4, LED_COUNT };

struct LedDef {
  uint8_t anode;
  uint8_t cathode;
  const char *name;
};

// LED1: Pin2 → Pin3, LED2: Pin2 → Pin4, LED3: Pin1 → Pin3, LED4: Pin1 → Pin4
const LedDef LEDS[LED_COUNT] = {
  { PIN_BOARD_2, PIN_BOARD_3, "LED1" },
  { PIN_BOARD_2, PIN_BOARD_4, "LED2" },
  { PIN_BOARD_1, PIN_BOARD_3, "LED3" },
  { PIN_BOARD_1, PIN_BOARD_4, "LED4" },
};

enum ButtonId { BTN_ESPRESSO_1, BTN_ESPRESSO_2, BTN_STEAM, BTN_COUNT };

struct ButtonDef {
  const char *name;
  char    code;     // код у протоколі до основної Arduino
  const char *path; // опис ланцюга для Serial
};

const ButtonDef BUTTONS[BTN_COUNT] = {
  { "1 ЕСПРЕСО", '1', "D8 → D4"  },
  { "2 ЕСПРЕСО", '2', "D8 → D12" },
  { "ПАР",       'S', "D12 → D10" },
};

// Який LED належить якій кнопці. ⚠ Гіпотеза за схемою — перевіряється на живій
// платі командою T (прогін LED1…LED4), а за потреби перепризначається командою
// X<кнопка><LED>, напр. X12 → кнопці 1 — LED2. LED4 кнопки не має, бере участь
// лише в чергуванні.
uint8_t ledForButton[BTN_COUNT] = { LED_1, LED_2, LED_3 };

// ─────────────────────────── Стан ───────────────────────────

SoftwareSerial link(LINK_RX_PIN, LINK_TX_PIN);

enum LedMode { LED_MODE_CHASE, LED_MODE_SOLID, LED_MODE_MANUAL, LED_MODE_WALK };

LedMode  ledMode = LED_MODE_CHASE;
int8_t   activeLed = LED_1;        // який LED зараз має горіти, -1 — жоден
uint8_t  chaseIndex = LED_1;
uint8_t  walkIndex = LED_1;
uint32_t lastChaseMs = 0;
uint32_t walkStepMs = 0;
uint32_t walkUntilMs = 0;
uint32_t holdUntilMs = 0;          // кінець SOLID / MANUAL
uint8_t  heldLed = LED_1;          // LED, який тримаємо в SOLID / MANUAL

bool     buttonState[BTN_COUNT];
uint8_t  pressStreak[BTN_COUNT];
uint8_t  releaseStreak[BTN_COUNT];

uint32_t lastScanMs = 0;

// ─────────────────────────── Прототипи ───────────────────────────

void   allPanelHighZ();
void   showActiveLed();
uint8_t readRawButtons();
void   scanButtons();
void   handlePress(uint8_t btn);
void   handleRelease(uint8_t btn);
void   sendLinkEvent(uint8_t btn, bool pressed);
void   updateIndicator();
void   enterChase();
void   handleSerialCommand();
void   executeCommand(const char *cmd);
void   startManualLed(uint8_t led);
void   startWalk();
void   remapLed(uint8_t btn, uint8_t led);
void   printMapping();
void   printHelp();

// ─────────────────────────── Setup / loop ───────────────────────────

void setup() {
#if USE_WATCHDOG
  wdt_disable();
#endif

  Serial.begin(PC_BAUD);
  link.begin(LINK_BAUD);   // SoftwareSerial::setTX сам ставить рівень HIGH до pinMode(OUTPUT)

  // RX від основної не потрібен (нам нема чого від неї приймати), а зайвий
  // PCINT на вільній лінії ловив би шум і з'їдав час у перериванні.
  link.stopListening();
  digitalWrite(LINK_RX_PIN, LOW);
  pinMode(LINK_RX_PIN, INPUT);

  allPanelHighZ();
  showActiveLed();

  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    buttonState[i]   = false;
    pressStreak[i]   = 0;
    releaseStreak[i] = 0;
  }

  lastChaseMs = millis();

  Serial.println();
  Serial.println(F("=== DeLonghi EC685: тест плати кнопок (007) ==="));
  printMapping();
  printHelp();

#if USE_WATCHDOG
  wdt_enable(WDTO_2S);
#endif
}

void loop() {
#if USE_WATCHDOG
  wdt_reset();
#endif

  handleSerialCommand();
  updateIndicator();
  // LED фізично перезапалюється наприкінці scanButtons() (кожні 5 мс) —
  // окремого applyIndicator() не потрібно.

  const uint32_t now = millis();
  if (now - lastScanMs >= SCAN_INTERVAL_MS) {
    lastScanMs = now;
    scanButtons();   // гасить LED → сканує → повертає індикацію
  }
}

// ─────────────────────────── Лінії плати ───────────────────────────

void allPanelHighZ() {
  // digitalWrite(LOW) ПЕРЕД pinMode(INPUT) обов'язковий: інакше на лінії,
  // яку раніше тягнули вгору (підтяжка), лишиться внутрішній pull-up ~30 кОм.
  digitalWrite(PIN_BOARD_1, LOW);
  digitalWrite(PIN_BOARD_2, LOW);
  digitalWrite(PIN_BOARD_3, LOW);
  digitalWrite(PIN_BOARD_4, LOW);
  digitalWrite(PIN_BOARD_5, LOW);
  digitalWrite(PIN_BOARD_6, LOW);

  pinMode(PIN_BOARD_1, INPUT);
  pinMode(PIN_BOARD_2, INPUT);
  pinMode(PIN_BOARD_3, INPUT);
  pinMode(PIN_BOARD_4, INPUT);
  pinMode(PIN_BOARD_5, INPUT);   // без підтяжки: інакше кнопки «засвічують» LED
  pinMode(PIN_BOARD_6, INPUT);
}

void showActiveLed() {
  allPanelHighZ();
  if (activeLed < 0) return;

  const LedDef &led = LEDS[activeLed];
  digitalWrite(led.anode, HIGH);     // фіксуємо рівень до перемикання в OUTPUT
  pinMode(led.anode, OUTPUT);
  digitalWrite(led.cathode, LOW);
  pinMode(led.cathode, OUTPUT);
}

// ─────────────────────────── Кнопки ───────────────────────────

// Два такти, як у 010_simple_button_test.cpp.
// D4 / D10 — INPUT зі зовнішньою підтяжкою до GND; HIGH = кнопка натиснута.
uint8_t readRawButtons() {
  allPanelHighZ();

  // Джерела за замовчуванням — OUTPUT LOW (безпечний спокій).
  digitalWrite(PIN_BOARD_2, LOW);
  pinMode(PIN_BOARD_2, OUTPUT);
  digitalWrite(PIN_BOARD_6, LOW);
  pinMode(PIN_BOARD_6, OUTPUT);

  pinMode(PIN_BOARD_4, INPUT);   // «пар», підтяжка до GND
  pinMode(PIN_BOARD_5, INPUT);   // «1 еспресо», підтяжка до GND

  // —— Такт 1: еспресо (D8 → D4 / D12) ——
  pinMode(PIN_BOARD_6, INPUT);   // D12 — приймач «2 еспресо»
  digitalWrite(PIN_BOARD_2, HIGH);
  delayMicroseconds(SETTLE_US);

  const bool raw1 = (digitalRead(PIN_BOARD_5) == HIGH);
  const bool raw2 = (digitalRead(PIN_BOARD_6) == HIGH);

  digitalWrite(PIN_BOARD_2, LOW);

  // —— Такт 2: пар (D12 → D10) ——
  pinMode(PIN_BOARD_6, OUTPUT);
  digitalWrite(PIN_BOARD_6, HIGH);
  delayMicroseconds(SETTLE_US);

  const bool rawSteam = (digitalRead(PIN_BOARD_4) == HIGH);

  digitalWrite(PIN_BOARD_6, LOW);

  // Як у 010: якщо одночасно HIGH на D4 і D12 — це «2 еспресо», не «1».
  uint8_t raw = 0;
  if (raw1 && !raw2) raw |= _BV(BTN_ESPRESSO_1);
  if (raw2)          raw |= _BV(BTN_ESPRESSO_2);
  if (rawSteam)      raw |= _BV(BTN_STEAM);
  return raw;
}

void scanButtons() {
  const uint8_t raw = readRawButtons();   // LED на цей час погашені

  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    const bool pressed = (raw & _BV(i)) != 0;

    if (pressed) {
      pressStreak[i]   = min((uint8_t)(pressStreak[i] + 1), DEBOUNCE_SAMPLES);
      releaseStreak[i] = 0;
    } else {
      releaseStreak[i] = min((uint8_t)(releaseStreak[i] + 1), DEBOUNCE_SAMPLES);
      pressStreak[i]   = 0;
    }

    if (!buttonState[i] && pressStreak[i] >= DEBOUNCE_SAMPLES) {
      buttonState[i] = true;
      handlePress(i);
    } else if (buttonState[i] && releaseStreak[i] >= DEBOUNCE_SAMPLES) {
      buttonState[i] = false;
      handleRelease(i);
    }
  }

  showActiveLed();   // повертаємо індикацію після сканування
}

void handlePress(uint8_t btn) {
  const uint8_t led = ledForButton[btn];

  ledMode     = LED_MODE_SOLID;
  heldLed     = led;
  holdUntilMs = millis() + SOLID_HOLD_MS;
  activeLed   = led;
  showActiveLed();

  Serial.print(F("["));
  Serial.print(millis());
  Serial.print(F(" ms] BTN «"));
  Serial.print(BUTTONS[btn].name);
  Serial.print(F("»: НАТИСНУТО → "));
  Serial.print(LEDS[led].name);
  Serial.print(F(" на "));
  Serial.print(SOLID_HOLD_MS);
  Serial.println(F(" мс"));

  sendLinkEvent(btn, true);
}

void handleRelease(uint8_t btn) {
  Serial.print(F("["));
  Serial.print(millis());
  Serial.print(F(" ms] BTN «"));
  Serial.print(BUTTONS[btn].name);
  Serial.println(F("»: ВІДПУЩЕНО"));

  sendLinkEvent(btn, false);
}

// Максимально коротко: SoftwareSerial передає байт з вимкненими перериваннями,
// тому довгий текст «заморожував» би millis() і мікросекундні таймери.
void sendLinkEvent(uint8_t btn, bool pressed) {
  link.write('B');
  link.write(BUTTONS[btn].code);
  link.write(pressed ? 'P' : 'R');
  link.write('\n');
}

// ─────────────────────────── Індикація ───────────────────────────

void enterChase() {
  ledMode     = LED_MODE_CHASE;
  lastChaseMs = millis();
}

void updateIndicator() {
  const uint32_t now = millis();

  switch (ledMode) {
    case LED_MODE_SOLID:
    case LED_MODE_MANUAL:
      if ((int32_t)(now - holdUntilMs) < 0) {
        activeLed = heldLed;
        return;
      }
      // після утримання — далі з наступного LED
      chaseIndex = (uint8_t)((heldLed + 1) % LED_COUNT);
      enterChase();
      break;

    case LED_MODE_WALK:
      if ((int32_t)(now - walkUntilMs) < 0) {
        if (now - walkStepMs >= WALK_STEP_MS) {
          walkStepMs = now;
          walkIndex  = (uint8_t)((walkIndex + 1) % LED_COUNT);
        }
        activeLed = walkIndex;
        return;
      }
      enterChase();
      break;

    default:
      break;
  }

  if (now - lastChaseMs >= CHASE_STEP_MS) {
    lastChaseMs = now;
    chaseIndex  = (uint8_t)((chaseIndex + 1) % LED_COUNT);
  }
  activeLed = chaseIndex;
}

// ─────────────────────────── Команди з ПК ───────────────────────────

void handleSerialCommand() {
  static char buf[8];
  static uint8_t len = 0;

  while (Serial.available() > 0) {
    const char c = (char)Serial.read();

    if (c == '\n' || c == '\r') {
      if (len > 0) {
        buf[len] = '\0';
        executeCommand(buf);
        len = 0;
      }
      continue;
    }

    if (len < sizeof(buf) - 1) {
      buf[len++] = c;
    }
  }
}

void executeCommand(const char *cmd) {
  switch (toupper((unsigned char)cmd[0])) {
    case 'L': {
      if (cmd[1] < '1' || cmd[1] > '4') {
        Serial.println(F("L — потрібно L1, L2, L3 або L4"));
        return;
      }
      startManualLed((uint8_t)(cmd[1] - '1'));
      return;
    }
    case 'T':
      startWalk();
      return;
    case 'A':
      enterChase();
      Serial.println(F("авто: чергування LED1…LED4"));
      return;
    case 'M':
      printMapping();
      return;
    case 'X': {
      if (cmd[1] < '1' || cmd[1] > '3' || cmd[2] < '1' || cmd[2] > '4') {
        Serial.println(F("X — потрібно X<кнопка 1…3><LED 1…4>, напр. X12"));
        return;
      }
      remapLed((uint8_t)(cmd[1] - '1'), (uint8_t)(cmd[2] - '1'));
      return;
    }
    default:
      printHelp();
      return;
  }
}

void startManualLed(uint8_t led) {
  ledMode     = LED_MODE_MANUAL;
  heldLed     = led;
  holdUntilMs = millis() + MANUAL_HOLD_MS;
  activeLed   = led;
  showActiveLed();

  Serial.print(F("L"));
  Serial.print(led + 1);
  Serial.print(F(" → "));
  Serial.print(LEDS[led].name);
  Serial.print(F(" (анод D"));
  Serial.print(LEDS[led].anode);
  Serial.print(F(", катод D"));
  Serial.print(LEDS[led].cathode);
  Serial.println(F(") на 5 с"));
}

void startWalk() {
  ledMode     = LED_MODE_WALK;
  walkIndex   = LED_1;
  activeLed   = walkIndex;
  walkStepMs  = millis();
  walkUntilMs = millis() + (uint32_t)WALK_STEP_MS * (LED_COUNT + 1);
  showActiveLed();

  Serial.println(F("T → прогін LED1…LED4 по 0.7 с"));
}

void remapLed(uint8_t btn, uint8_t led) {
  ledForButton[btn] = led;
  Serial.print(F("«"));
  Serial.print(BUTTONS[btn].name);
  Serial.print(F("» → "));
  Serial.println(LEDS[led].name);
  printMapping();
}

void printMapping() {
  Serial.println(F("--- Розводка ---"));
  for (uint8_t b = 0; b < BTN_COUNT; b++) {
    const LedDef &led = LEDS[ledForButton[b]];
    Serial.print(F("«"));
    Serial.print(BUTTONS[b].name);
    Serial.print(F("» ["));
    Serial.print(BUTTONS[b].code);
    Serial.print(F("] "));
    Serial.print(BUTTONS[b].path);
    Serial.print(F(" → "));
    Serial.print(led.name);
    Serial.print(F(" (анод D"));
    Serial.print(led.anode);
    Serial.print(F(", катод D"));
    Serial.print(led.cathode);
    Serial.println(F(")"));
  }
  Serial.print(F("LED4 — без кнопки, лише в чергуванні (анод D"));
  Serial.print(PIN_BOARD_1);
  Serial.print(F(", катод D"));
  Serial.print(PIN_BOARD_4);
  Serial.println(F(")"));
  Serial.print(F("Піни плати: D7/D8/D9/D10/D4/D12; лінк D6 → D9 основної, "));
  Serial.print(LINK_BAUD);
  Serial.println(F(" бод"));
}

void printHelp() {
  Serial.println(F("Команди: L1|L2|L3|L4 — примусово LED | T — прогін | A — авто |"));
  Serial.println(F("         M — розводка | X<кнопка><LED> — перепризначити LED | ? — довідка"));
}

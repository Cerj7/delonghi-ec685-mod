// Button Panel UI — 011_button_panel_ui.cpp
//
// Робоча індикація штатної плати кнопок DeLonghi EC685 за командами
// основної Arduino. Апаратний шар кнопок/LED — як у 007 / 010
// (цифрове читання зі зовнішніми стягуючими резисторами на D4/D10).
//
//   Pin1 → D7    аноди LED3 / LED4
//   Pin2 → D8    аноди LED1 / LED2 + спільний вузол кнопок (через діод)
//   Pin3 → D9    катоди LED1 / LED3
//   Pin4 → D10   катоди LED2 / LED4 + приймач «пар»
//   Pin5 → D4    приймач «1 еспресо»
//   Pin6 → D12   приймач «2 еспресо» / джерело для «пар»
//
// Двобічний UART з основною Nano (контракт — docs/button-panel-protocol.md):
//   D6 TX → D9 основної,  D5 RX ← D8 основної,  19200 8N1
//   Панель → основна: B1 / B2 / B3  (фронт натискання)
//   Основна → панель: HE1 HE0 P11 P10 P21 P20 HS1 HS0 SS1 SS0 DC1 DC0
//
// LED: початково всі вимкнені. Кілька LED одночасно — мультиплексування
// по одному на такт сканування (~5 мс). Мигання — 300 мс on/off.
//
// Бібліотеки: SoftwareSerial з AVR core.

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <avr/wdt.h>
#include <ctype.h>
#include <string.h>

// ─────────────────────────── Налаштування ───────────────────────────

#define USE_WATCHDOG 1

const uint8_t PIN_BOARD_1 = 7;
const uint8_t PIN_BOARD_2 = 8;
const uint8_t PIN_BOARD_3 = 9;
const uint8_t PIN_BOARD_4 = 10;
const uint8_t PIN_BOARD_5 = 4;
const uint8_t PIN_BOARD_6 = 12;

const uint8_t LINK_RX_PIN = 5;    // D5 ← D8 основної
const uint8_t LINK_TX_PIN = 6;    // D6 → D9 основної
const long    LINK_BAUD   = 19200;
const long    PC_BAUD     = 115200;

const uint16_t SCAN_INTERVAL_MS = 5;
const uint16_t SETTLE_US        = 20;
const uint8_t  DEBOUNCE_SAMPLES = 3;     // 3 × 5 мс = 15 мс
const uint16_t BLINK_HALF_MS    = 300;

// ─────────────────────────── Таблиці ───────────────────────────

enum LedId { LED_1, LED_2, LED_3, LED_4, LED_COUNT };

enum LedVis { LED_OFF = 0, LED_SOLID = 1, LED_BLINK = 2 };

struct LedDef {
  uint8_t anode;
  uint8_t cathode;
  const char *name;
};

const LedDef LEDS[LED_COUNT] = {
  { PIN_BOARD_2, PIN_BOARD_3, "LED1" },
  { PIN_BOARD_2, PIN_BOARD_4, "LED2" },
  { PIN_BOARD_1, PIN_BOARD_3, "LED3" },
  { PIN_BOARD_1, PIN_BOARD_4, "LED4" },
};

enum ButtonId { BTN_ESPRESSO_1, BTN_ESPRESSO_2, BTN_STEAM, BTN_COUNT };

struct ButtonDef {
  const char *name;
  char code;   // '1' / '2' / '3' у кадрі B?
};

const ButtonDef BUTTONS[BTN_COUNT] = {
  { "1 ЕСПРЕСО", '1' },
  { "2 ЕСПРЕСО", '2' },
  { "ПАР",       '3' },
};

// ─────────────────────────── Стан ───────────────────────────

SoftwareSerial link(LINK_RX_PIN, LINK_TX_PIN);

LedVis   ledMode[LED_COUNT];
bool     brewing1 = false;
bool     brewing2 = false;
bool     steamHeating = false;
bool     steamSupplying = false;
bool     descalingNeeded = false;

bool     buttonState[BTN_COUNT];
uint8_t  pressStreak[BTN_COUNT];
uint8_t  releaseStreak[BTN_COUNT];

uint8_t  ledMuxIndex = 0;
uint32_t lastScanMs = 0;

char     linkBuf[8];
uint8_t  linkLen = 0;

char     pcBuf[8];
uint8_t  pcLen = 0;

// ─────────────────────────── Прототипи ───────────────────────────

void allPanelHighZ();
void driveLed(uint8_t led);
void applyLedMux();
bool ledShouldShow(uint8_t led);
void refreshLed4();
void setEspressoLeds(LedVis mode);
uint8_t readRawButtons();
void scanButtons();
void handlePress(uint8_t btn);
void sendButtonEvent(uint8_t btn);
void handleLinkByte(char c);
void executeLinkCommand(const char *cmd);
void handlePcSerial();
void executePcCommand(const char *cmd);
void printStatus();
void printHelp();

// ─────────────────────────── Setup / loop ───────────────────────────

void setup() {
#if USE_WATCHDOG
  wdt_disable();
#endif

  Serial.begin(PC_BAUD);
  link.begin(LINK_BAUD);

  for (uint8_t i = 0; i < LED_COUNT; i++) {
    ledMode[i] = LED_OFF;
  }

  for (uint8_t i = 0; i < BTN_COUNT; i++) {
    buttonState[i]   = false;
    pressStreak[i]   = 0;
    releaseStreak[i] = 0;
  }

  allPanelHighZ();

  Serial.println();
  Serial.println(F("=== DeLonghi EC685: панель кнопок UI (011) ==="));
  Serial.println(F("Протокол: docs/button-panel-protocol.md"));
  printHelp();
  printStatus();

#if USE_WATCHDOG
  wdt_enable(WDTO_2S);
#endif
}

void loop() {
#if USE_WATCHDOG
  wdt_reset();
#endif

  while (link.available() > 0) {
    handleLinkByte((char)link.read());
  }

  handlePcSerial();

  const uint32_t now = millis();
  if (now - lastScanMs >= SCAN_INTERVAL_MS) {
    lastScanMs = now;
    scanButtons();   // гасить LED → сканує → applyLedMux()
  }
}

// ─────────────────────────── Лінії плати ───────────────────────────

void allPanelHighZ() {
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
  pinMode(PIN_BOARD_5, INPUT);
  pinMode(PIN_BOARD_6, INPUT);
}

void driveLed(uint8_t led) {
  allPanelHighZ();
  if (led >= LED_COUNT) return;

  const LedDef &def = LEDS[led];
  digitalWrite(def.anode, HIGH);
  pinMode(def.anode, OUTPUT);
  digitalWrite(def.cathode, LOW);
  pinMode(def.cathode, OUTPUT);
}

bool ledShouldShow(uint8_t led) {
  const LedVis mode = ledMode[led];
  if (mode == LED_OFF) return false;
  if (mode == LED_SOLID) return true;
  // LED_BLINK
  return ((millis() / BLINK_HALF_MS) & 1u) == 0u;
}

void applyLedMux() {
  // Шукаємо наступний LED, який має світитись у цьому кадрі.
  for (uint8_t n = 0; n < LED_COUNT; n++) {
    ledMuxIndex = (uint8_t)((ledMuxIndex + 1) % LED_COUNT);
    if (ledShouldShow(ledMuxIndex)) {
      driveLed(ledMuxIndex);
      return;
    }
  }
  allPanelHighZ();
}

void refreshLed4() {
  const bool blocked = brewing1 || brewing2 || steamHeating || steamSupplying;
  if (descalingNeeded && !blocked) {
    ledMode[LED_4] = LED_BLINK;
  } else {
    ledMode[LED_4] = LED_OFF;
  }
}

void setEspressoLeds(LedVis mode) {
  ledMode[LED_1] = mode;
  ledMode[LED_2] = mode;
}

// ─────────────────────────── Кнопки ───────────────────────────

uint8_t readRawButtons() {
  allPanelHighZ();

  digitalWrite(PIN_BOARD_2, LOW);
  pinMode(PIN_BOARD_2, OUTPUT);
  digitalWrite(PIN_BOARD_6, LOW);
  pinMode(PIN_BOARD_6, OUTPUT);

  pinMode(PIN_BOARD_4, INPUT);
  pinMode(PIN_BOARD_5, INPUT);

  // Такт 1: еспресо (D8 → D4 / D12)
  pinMode(PIN_BOARD_6, INPUT);
  digitalWrite(PIN_BOARD_2, HIGH);
  delayMicroseconds(SETTLE_US);

  const bool raw1 = (digitalRead(PIN_BOARD_5) == HIGH);
  const bool raw2 = (digitalRead(PIN_BOARD_6) == HIGH);

  digitalWrite(PIN_BOARD_2, LOW);

  // Такт 2: пар (D12 → D10)
  pinMode(PIN_BOARD_6, OUTPUT);
  digitalWrite(PIN_BOARD_6, HIGH);
  delayMicroseconds(SETTLE_US);

  const bool rawSteam = (digitalRead(PIN_BOARD_4) == HIGH);

  digitalWrite(PIN_BOARD_6, LOW);

  uint8_t raw = 0;
  if (raw1 && !raw2) raw |= _BV(BTN_ESPRESSO_1);
  if (raw2)          raw |= _BV(BTN_ESPRESSO_2);
  if (rawSteam)      raw |= _BV(BTN_STEAM);
  return raw;
}

void scanButtons() {
  const uint8_t raw = readRawButtons();

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
    }
  }

  applyLedMux();
}

void handlePress(uint8_t btn) {
  Serial.print(F("["));
  Serial.print(millis());
  Serial.print(F(" ms] BTN «"));
  Serial.print(BUTTONS[btn].name);
  Serial.println(F("» → лінк"));

  sendButtonEvent(btn);
}

void sendButtonEvent(uint8_t btn) {
  link.write('B');
  link.write(BUTTONS[btn].code);
  link.write('\n');
}

// ─────────────────────────── UART від основної ───────────────────────────

void handleLinkByte(char c) {
  if (c == '\r') return;

  if (c == '\n') {
    if (linkLen > 0) {
      linkBuf[linkLen] = '\0';
      executeLinkCommand(linkBuf);
      linkLen = 0;
    }
    return;
  }

  if (linkLen < sizeof(linkBuf) - 1) {
    linkBuf[linkLen++] = (char)toupper((unsigned char)c);
  } else {
    linkLen = 0;   // переповнення — чекаємо наступний кадр
  }
}

void executeLinkCommand(const char *cmd) {
  Serial.print(F("← "));
  Serial.println(cmd);

  if (strcmp(cmd, "HE1") == 0) {
    setEspressoLeds(LED_BLINK);
  } else if (strcmp(cmd, "HE0") == 0) {
    setEspressoLeds(LED_SOLID);
  } else if (strcmp(cmd, "P11") == 0) {
    brewing1 = true;
    ledMode[LED_1] = LED_BLINK;
    refreshLed4();
  } else if (strcmp(cmd, "P10") == 0) {
    brewing1 = false;
    ledMode[LED_1] = LED_SOLID;
    refreshLed4();
  } else if (strcmp(cmd, "P21") == 0) {
    brewing2 = true;
    ledMode[LED_2] = LED_BLINK;
    refreshLed4();
  } else if (strcmp(cmd, "P20") == 0) {
    brewing2 = false;
    ledMode[LED_2] = LED_SOLID;
    refreshLed4();
  } else if (strcmp(cmd, "HS1") == 0) {
    steamHeating = true;
    steamSupplying = false;
    ledMode[LED_3] = LED_BLINK;
    refreshLed4();
  } else if (strcmp(cmd, "HS0") == 0) {
    steamHeating = false;
    ledMode[LED_3] = LED_SOLID;
    refreshLed4();
  } else if (strcmp(cmd, "SS1") == 0) {
    steamSupplying = true;
    steamHeating = false;
    ledMode[LED_3] = LED_BLINK;
    refreshLed4();
  } else if (strcmp(cmd, "SS0") == 0) {
    steamSupplying = false;
    ledMode[LED_3] = LED_SOLID;
    refreshLed4();
  } else if (strcmp(cmd, "DC1") == 0) {
    descalingNeeded = true;
    refreshLed4();
  } else if (strcmp(cmd, "DC0") == 0) {
    descalingNeeded = false;
    refreshLed4();
  } else {
    Serial.print(F("невідома команда: "));
    Serial.println(cmd);
    return;
  }

  printStatus();
}

// ─────────────────────────── USB (налагодження) ───────────────────────────

void handlePcSerial() {
  while (Serial.available() > 0) {
    const char c = (char)Serial.read();

    if (c == '\r') continue;

    if (c == '\n') {
      if (pcLen > 0) {
        pcBuf[pcLen] = '\0';
        executePcCommand(pcBuf);
        pcLen = 0;
      }
      continue;
    }

    if (pcLen < sizeof(pcBuf) - 1) {
      pcBuf[pcLen++] = c;
    } else {
      pcLen = 0;
    }
  }
}

void executePcCommand(const char *cmd) {
  if (cmd[0] == '?' || cmd[0] == 'H' || cmd[0] == 'h') {
    // «H» конфліктує з HE* — лише одиночний H/? як довідка
    if (cmd[1] == '\0' || cmd[0] == '?') {
      printHelp();
      return;
    }
  }

  if ((cmd[0] == 'S' || cmd[0] == 's') && cmd[1] == '\0') {
    printStatus();
    return;
  }

  // Локальний прогін тих самих кодів, що й від основної (без лінка)
  char upper[8];
  uint8_t n = 0;
  while (cmd[n] != '\0' && n < sizeof(upper) - 1) {
    upper[n] = (char)toupper((unsigned char)cmd[n]);
    n++;
  }
  upper[n] = '\0';
  executeLinkCommand(upper);
}

void printStatus() {
  Serial.print(F("LED:"));
  for (uint8_t i = 0; i < LED_COUNT; i++) {
    Serial.print(' ');
    Serial.print(LEDS[i].name);
    Serial.print('=');
    if (ledMode[i] == LED_OFF) Serial.print(F("off"));
    else if (ledMode[i] == LED_SOLID) Serial.print(F("on"));
    else Serial.print(F("blink"));
  }
  Serial.print(F(" | brew1="));
  Serial.print(brewing1 ? 1 : 0);
  Serial.print(F(" brew2="));
  Serial.print(brewing2 ? 1 : 0);
  Serial.print(F(" steamH="));
  Serial.print(steamHeating ? 1 : 0);
  Serial.print(F(" steamS="));
  Serial.print(steamSupplying ? 1 : 0);
  Serial.print(F(" descale="));
  Serial.println(descalingNeeded ? 1 : 0);
}

void printHelp() {
  Serial.println(F("Лінк 19200: B1/B2/B3 → основна; ← HE1 HE0 P11 P10 P21 P20 HS1 HS0 SS1 SS0 DC1 DC0"));
  Serial.println(F("USB: ті самі коди (локально) | S — статус | ? — довідка"));
}

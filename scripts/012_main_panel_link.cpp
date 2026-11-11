// Main ↔ Button Panel link stub — 012_main_panel_link.cpp
//
// Тестова заглушка «основної» Arduino Nano: приймає події кнопок з панелі
// і надсилає команди індикації. Реальної логіки термоблока/помпи немає —
// лише прозорий міст USB ↔ UART для перевірки контракту
// docs/button-panel-protocol.md.
//
// Піни (як у docs/buttons.md):
//   D9 RX ← D6 плати кнопок
//   D8 TX → D5 плати кнопок
//   спільний GND між двома Nano
//
// USB Serial 115200:
//   вхід  B1/B2/B3 з панелі — друкується як «BTN …»
//   вихід: наберіть HE1, HE0, P11, … — кадр піде на панель
//
// Автосимуляція (команда A): після B1/B2/B3 сама шле типову відповідь
// з паузами, щоб можна було ганяти стенд однією рукою.

#include <Arduino.h>
#include <SoftwareSerial.h>
#include <ctype.h>
#include <string.h>

const uint8_t PANEL_RX_PIN = 9;   // ← D6 панелі
const uint8_t PANEL_TX_PIN = 8;   // → D5 панелі
const long    LINK_BAUD    = 19200;
const long    PC_BAUD      = 115200;

SoftwareSerial panel(PANEL_RX_PIN, PANEL_TX_PIN);

char linkBuf[8];
uint8_t linkLen = 0;

char pcBuf[12];
uint8_t pcLen = 0;

bool autoReply = true;

// Проста черга відкладених відповідей для режиму A
struct Pending {
  uint32_t atMs;
  const char *code;
  bool used;
};

Pending pending[4];

void sendToPanel(const char *code);
void handlePanelByte(char c);
void onButton(char code);
void handlePcSerial();
void executePcCommand(const char *cmd);
void queueReply(const char *code, uint16_t delayMs);
void servicePending();
void printHelp();

void setup() {
  Serial.begin(PC_BAUD);
  panel.begin(LINK_BAUD);

  for (uint8_t i = 0; i < 4; i++) {
    pending[i].used = false;
  }

  Serial.println();
  Serial.println(F("=== DeLonghi EC685: заглушка основної ↔ панель (012) ==="));
  Serial.println(F("Протокол: docs/button-panel-protocol.md"));
  printHelp();
}

void loop() {
  while (panel.available() > 0) {
    handlePanelByte((char)panel.read());
  }

  handlePcSerial();
  servicePending();
}

void sendToPanel(const char *code) {
  panel.print(code);
  panel.write('\n');

  Serial.print(F("→ панель "));
  Serial.println(code);
}

void handlePanelByte(char c) {
  if (c == '\r') return;

  if (c == '\n') {
    if (linkLen > 0) {
      linkBuf[linkLen] = '\0';

      if (linkLen == 2 && linkBuf[0] == 'B') {
        Serial.print(F("← панель BTN "));
        Serial.println(linkBuf[1]);
        onButton(linkBuf[1]);
      } else {
        Serial.print(F("← панель (невідомо) "));
        Serial.println(linkBuf);
      }

      linkLen = 0;
    }
    return;
  }

  if (linkLen < sizeof(linkBuf) - 1) {
    linkBuf[linkLen++] = c;
  } else {
    linkLen = 0;
  }
}

void onButton(char code) {
  if (!autoReply) return;

  switch (code) {
    case '1':
      // типовий сценарій: старт проливу 1, через 3 с — кінець
      sendToPanel("P11");
      queueReply("P10", 3000);
      break;
    case '2':
      sendToPanel("P21");
      queueReply("P20", 3000);
      break;
    case '3':
      // чергуємо нагрів пару і подачу: якщо вже «чекаємо» HS0 у черзі —
      // це спрощення; для демо: спочатку HS, потім при наступному B3 — SS
      {
        static bool steamHeated = false;
        if (!steamHeated) {
          sendToPanel("HS1");
          queueReply("HS0", 2500);
          steamHeated = true;
        } else {
          sendToPanel("SS1");
          queueReply("SS0", 2500);
          steamHeated = false;
        }
      }
      break;
    default:
      break;
  }
}

void queueReply(const char *code, uint16_t delayMs) {
  for (uint8_t i = 0; i < 4; i++) {
    if (!pending[i].used) {
      pending[i].used = true;
      pending[i].code = code;
      pending[i].atMs = millis() + delayMs;
      return;
    }
  }
  Serial.println(F("черга відповідей повна"));
}

void servicePending() {
  const uint32_t now = millis();
  for (uint8_t i = 0; i < 4; i++) {
    if (!pending[i].used) continue;
    if ((int32_t)(now - pending[i].atMs) < 0) continue;
    sendToPanel(pending[i].code);
    pending[i].used = false;
  }
}

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
  // Одиночні керуючі
  if (strcmp(cmd, "?") == 0 || strcmp(cmd, "h") == 0 || strcmp(cmd, "H") == 0) {
    printHelp();
    return;
  }
  if (strcmp(cmd, "A") == 0 || strcmp(cmd, "a") == 0) {
    autoReply = true;
    Serial.println(F("авто-відповіді УВІМК"));
    return;
  }
  if (strcmp(cmd, "M") == 0 || strcmp(cmd, "m") == 0) {
    autoReply = false;
    Serial.println(F("авто-відповіді ВИМК — лише ручні коди"));
    return;
  }

  // Нормалізація регістру і форвард на панель
  char upper[12];
  uint8_t n = 0;
  while (cmd[n] != '\0' && n < sizeof(upper) - 1) {
    upper[n] = (char)toupper((unsigned char)cmd[n]);
    n++;
  }
  upper[n] = '\0';

  // Відомі коди протоколу
  static const char *const KNOWN[] = {
    "HE1", "HE0",
    "P11", "P10", "P21", "P20",
    "HS1", "HS0", "SS1", "SS0",
    "LT1", "LT0",
    "DC1", "DC0",
  };

  bool ok = false;
  for (uint8_t i = 0; i < sizeof(KNOWN) / sizeof(KNOWN[0]); i++) {
    if (strcmp(upper, KNOWN[i]) == 0) {
      ok = true;
      break;
    }
  }

  if (!ok) {
    Serial.print(F("невідомо: "));
    Serial.println(cmd);
    printHelp();
    return;
  }

  sendToPanel(upper);
}

void printHelp() {
  Serial.println(F("Команди на панель: HE1 HE0 P11 P10 P21 P20 HS1 HS0 SS1 SS0 LT1 LT0 DC1 DC0"));
  Serial.println(F("A — авто-відповіді на B1/B2/B3 (увімк за замовч.) | M — лише ручні"));
  Serial.println(F("? — довідка | лінк D9←D6, D8→D5, 19200"));
}

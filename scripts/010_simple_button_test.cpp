const uint8_t PIN_D8  = 8;   // Джерело для кнопок 1 та 2 Еспресо
const uint8_t PIN_D12 = 12;  // Джерело для кнопки Пар (такт 2) / Приймач для кнопки 2 (такт 1)
const uint8_t PIN_D4 = 4;  // Приймач для кнопки 1 Еспресо
const uint8_t PIN_D10 = 10;  // Приймач для кнопки Пар

void setup() {
  Serial.begin(115200);
  Serial.println("Система DeLonghi EC685 з апаратними резисторами активна.");
  
  // Налаштування пінів-джерел
  pinMode(PIN_D8, OUTPUT);
  digitalWrite(PIN_D8, LOW);
  
  pinMode(PIN_D12, OUTPUT);
  digitalWrite(PIN_D12, LOW);

  // Піни D10 та D11 завжди працюють як входи (стягнуті резисторами до GND)
  pinMode(PIN_D10, INPUT);
  pinMode(PIN_D4, INPUT);
}

void loop() {
  bool btn1 = false;
  bool btn2 = false;
  bool btnSteam = false;

  // ==========================================
  // ТАКТ 1: Опитування еспресо (D8 -> D11 та D12)
  // ==========================================
  
  // D12 на цей такт стає входом (приймач для кнопки 2)
  pinMode(PIN_D12, INPUT);

  // Подаємо HIGH на D8
  digitalWrite(PIN_D8, HIGH);
  delayMicroseconds(20); // Коротка стабілізація через діоди

  bool raw1 = (digitalRead(PIN_D4) == HIGH);
  bool raw2 = (digitalRead(PIN_D12) == HIGH);

  // Вимикаємо D8
  digitalWrite(PIN_D8, LOW);

  btn1 = raw1 && !raw2;
  btn2 = raw2;


  // ==========================================
  // ТАКТ 2: Опитування кнопки "Пар" (D12 -> D10)
  // ==========================================
  
  // D12 на цей такт стає джерелом для кнопки Пар
  pinMode(PIN_D12, OUTPUT);
  digitalWrite(PIN_D12, HIGH);
  delayMicroseconds(20);

  // Читаємо стан на D10
  btnSteam = (digitalRead(PIN_D10) == HIGH);

  // Вимикаємо D12 і повертаємо його в безпечний LOW (OUTPUT)
  digitalWrite(PIN_D12, LOW);


  // --- ВИВІД РЕЗУЛЬТАТІВ У SERIAL MONITOR (тільки при зміні стану) ---
  static bool prev1 = false, prev2 = false, prevSteam = false;
  if (btn1 != prev1 || btn2 != prev2 || btnSteam != prevSteam) {
    prev1 = btn1;
    prev2 = btn2;
    prevSteam = btnSteam;
    
    Serial.print("1 Еспресо: ");
    Serial.print(btn1 ? "НАТИСНУТО" : "-");
    Serial.print(" | 2 Еспресо: ");
    Serial.print(btn2 ? "НАТИСНУТО" : "-");
    Serial.print(" | Пар: ");
    Serial.println(btnSteam ? "НАТИСНУТО" : "-");
  }

  delay(30); // Коротка пауза між циклами
}
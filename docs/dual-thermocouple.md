# Dual Thermocouple — режим вимірів (`scripts/004_*`)

Скетч і супутній Python-плотер для **лише вимірювання** двох термопар MAX6675 на DeLonghi EC685. Крок 9 у [`readme.md`](../readme.md). Помпа, ТЕН, кнопки та диммер **не** керуються.

| Файл | Роль |
| --- | --- |
| [`scripts/004_dual_thermocouple.cpp`](../scripts/004_dual_thermocouple.cpp) | Прошивка Nano: зчитування Tt/Tw, OLED, CSV у Serial |
| [`scripts/004_plot_temps.py`](../scripts/004_plot_temps.py) | Live-графік Tt на ПК (вікно 60 с) + опційне збереження CSV |

---

## Навіщо

1. Перевірити обидва модулі MAX6675 і розводку CS/SO/SCK.
2. Бачити **Tt** (термоблок) і **Tw** (вода в бачку) на OLED під час роботи машини.
3. Будувати на ПК графік **Tt** за останню хвилину (прогрів, пролив, охолодження).
4. Зберігати логи для подальшого калібрування моделі нагріву (див. [`theory.md`](theory.md)).

Це не робоча прошивка кавоварки — лише стенд вимірів.

---

## Позначення

| Ім’я | Фізичний сенс | Частота | На графіку |
| --- | --- | --- | --- |
| **Tt** | Температура термоблока / виходу | ~кожні 250 мс | так (крива) |
| **Tw** | Температура води в бачку | кожні **10 с** | ні (підпис / OLED) |

Графік на OLED **не** малюється: дисплей 128×32 замалий для хвилинної історії. Історія Tt будується на ПК.

---

## Arduino: `004_dual_thermocouple.cpp`

### Що робить

| Блок | Поведінка |
| --- | --- |
| **Tt** | `readCelsius()` кожні 250 мс; значення на OLED і в Serial |
| **Tw** | Перше зчитування в `setup`, далі кожні 10 с; на OLED; у Serial як рядок-коментар |
| **OLED** | Два рядки `setTextSize(2)`: `Tt=xx.xC` / `Tw=xx.xC` |
| **Serial** | `115200`; див. формат нижче |
| **Керування** | Немає (немає диммера, SSR, флоуметра) |

### Піни

| Пін | Роль |
| --- | --- |
| `A2` | CS термопари **Tt** (термоблок) |
| `D11` | CS термопари **Tw** (бачок) |
| `D12` | SO / MISO (спільна шина) |
| `D13` | SCK (спільна шина) |
| `A4` / `A5` | OLED I2C (SDA / SCL) |

Повна таблиця pinout — у [`readme.md`](../readme.md). Модулі незалежні по CS: повільне опитування Tw не збиває період Tt. MAX6675 потребує ≥220 мс після `read()` того ж чипа.

### Бібліотеки (Arduino IDE)

1. [Adafruit GFX](https://github.com/adafruit/Adafruit-GFX-Library)
2. [Adafruit SSD1306](https://github.com/adafruit/Adafruit_SSD1306)
3. [MAX6675 library (Adafruit)](https://github.com/adafruit/MAX6675-library) — у Library Manager шукати **MAX6675**, автор Adafruit

Файл `.cpp`: відкрити в IDE як скетч або скопіювати вміст у `.ino`.

### Прошивка

1. Встановити бібліотеки.
2. Board: **Arduino Nano**, правильний порт.
3. Закрити Serial Monitor / Plotter / Python-плотер (інакше порт зайнятий).
4. Upload скетча.

### Формат Serial

Рядки з `#` — коментарі (Serial Plotter їх ігнорує; Python парсить `# Tw,...`).

```text
# EC685 dual-thermocouple (measure-only)
# Tt=thermoblock (plot)  Tw=water every 10s (not plotted)
# t_ms,Tt_C
12340,91.25
12590,91.50
# Tw,13000,22.75
12840,91.75
```

| Рядок | Опис |
| --- | --- |
| `t_ms,Tt_C` | Кожні ~250 мс. Єдина крива для Serial Plotter |
| `# Tw,<t_ms>,<Tw_C>` | Кожні ~10 с (і одразу після старту) |

`nan` у полі температури = помилка зчитування (обрив термопари, CS/SO/SCK, живлення).

### Швидкий графік без Python

Arduino IDE → **Tools → Serial Plotter**, baud **115200**. Побачите одну криву Tt. Tw на графік не потрапить.

---

## Python: `004_plot_temps.py`

### Що робить

- Читає Serial з прошитого Nano.
- Малює **live-графік Tt** з ковзним вікном (за замовчуванням **60 с**).
- Показує поточне **Tw** підписом на графіку (не кривою).
- З `--save` пише CSV: `logs/temps_YYYYMMDD_HHMMSS.csv`.

### Залежності

```bash
python3 -m venv plotter_venv
source plotter_venv/bin/activate
pip install pyserial matplotlib PyQt5
```

`PyQt5` (або системний `python3-tk`) потрібен для **вікна** графіка. Без GUI-бекенду matplotlib лишається на Agg → попередження `FigureCanvasAgg is non-interactive` і вікно не відкриється (див. розділ «Типові проблеми»).

### Запуск

```bash
cd /mnt/data/Programs/delonghi-ec685-mod/
source plotter_venv/bin/activate
python3 scripts/004_plot_temps.py --list-ports
python3 scripts/004_plot_temps.py --port /dev/ttyUSB0
python3 scripts/004_plot_temps.py --port /dev/ttyUSB0 --save
python3 scripts/004_plot_temps.py --port /dev/ttyUSB0 --save --outdir logs --window 60
```

| Аргумент | За замовчуванням | Опис |
| --- | --- | --- |
| `--port` | — | Обов’язковий (окрім `--list-ports`) |
| `--baud` | `115200` | Має збігатися зі скетчем |
| `--window` | `60` | Довжина історії Tt, секунди |
| `--save` | вимкнено | Писати CSV під час роботи |
| `--outdir` | `logs` | Каталог для CSV |
| `--list-ports` | — | Показати порти й вийти |

Після старту: рядок на кшталт `Графік: бекенд QtAgg. Закрийте вікно, щоб вийти.` Закриття вікна завершує скрипт і закриває CSV.

### Формат збереженого CSV

```text
t_ms,Tt_C,Tw_C,wall_time
12340,91.25,22.75,2026-09-21T09:11:42.123
12590,91.50,22.75,2026-09-21T09:11:42.373
```

| Поле | Опис |
| --- | --- |
| `t_ms` | `millis()` з Arduino |
| `Tt_C` | Температура термоблока на цьому семплі |
| `Tw_C` | Останнє відоме Tw (повторюється до наступного `# Tw,...`) |
| `wall_time` | Час ПК (ISO) |

---

## Рекомендований порядок роботи

1. Прошити `004_dual_thermocouple.cpp`.
2. На OLED перевірити, що Tt і Tw не `---`.
3. Закрити Serial Monitor у IDE.
4. Запустити `004_plot_temps.py --port … --save`.
5. Прогнати сценарій (прогрів / пролив) → закрити вікно → аналізувати CSV.

---

## Типові проблеми

| Симптом | Ймовірна причина | Що зробити |
| --- | --- | --- |
| `max6675.h: No such file` | Немає бібліотеки | Library Manager → **MAX6675** (Adafruit) |
| OLED `---` / Serial `nan` | Обрив / переплутані CS / немає живлення модуля | Перевірити `D11`/`A2`/`D12`/`D13`, VCC/GND, холодний спай |
| Tw застряг, Tt ок | Лише CS Tw (`D11`) або модуль #2 | Поміняти місцями модулі для діагностики |
| Upload / open port failed | Порт зайнятий Plotter’ом або Python | Закрити вікно / Monitor |
| `FigureCanvasAgg is non-interactive` | У venv немає Qt/Tk | `pip install PyQt5` у тому ж venv |
| Порт не той | Nano на іншому `/dev/ttyUSBx` | `--list-ports` |
| Nano «ресетиться» при старті плотера | Нормально для USB-serial | Перші рядки після reset можуть бути з `#` |

---

## Обмеження

- Режим **measure-only**: штатний MCU і помпа не перехоплюються.
- Роздільна здатність MAX6675 ~0.25 °C; період конверсії обмежує частоту Tt (~4 Гц).
- Tw раз на 10 с достатньо для бачка (повільна зміна), але не для швидких перехідних процесів у воді.

---

## Пов’язані файли

- [`hardware.md`](../hardware.md) — перелік компонентів
- [`docs/display.md`](display.md) — OLED 0.91"
- [`docs/theory.md`](theory.md) — навіщо Tt / Tw у моделі нагріву
- [`docs/debug-repeater.md`](debug-repeater.md) — попередній діагностичний скетч (одна термопара + SDD)

# Contract: Panel UART (Main Brew Logic consumer)

Джерело істини протоколу: [`docs/button-panel-protocol.md`](../../../docs/button-panel-protocol.md).

Цей файл фіксує **поведінку основної плати** як споживача/виробника кадрів у фічі `001-main-brew-logic` (де вона розходиться з ілюстративним прикладом у протоколі).

**Прошивка панелі `011` у цій фічі не змінюється** (clarify session 4).

## Link

| Param | Value |
| --- | --- |
| UART | 8N1, 19200 |
| Main RX | D9 ← Panel D6 |
| Main TX | D8 → Panel D5 |
| Frame | ASCII code ≤3 chars + `\n` |
| USB debug | 115200 (не частина міжплатного контракту) |

## Ingress (panel → main)

| Code | Meaning | Main action (by state) |
| --- | --- | --- |
| `B1` | «1 еспресо» | З `ESPRESSO_READY` → cold flush (`P11`); у `COLD_FLUSH` → stop (`P10`); у `HS1`/`HS0`/`SS*` → `coolOrReady()`/`LT1`; у `BREW_2`/`STEAM_COOL`/`ESPRESSO_HEAT` → ignore |
| `B2` | «2 еспресо» | З `ESPRESSO_READY` → brew 2 (`P21`); у `BREW_2` → stop (`P20`); у `HS1`/`HS0`/`SS*` → `coolOrReady()`/`LT1`; інакше ignore за матрицею FSM |
| `B3` | «пар» | З ready/cool → `enterSteam()`; у `HS*`/`SS*` → **ignore** (не cancel); ignore під час heat/flush/brew |

## Egress (main → panel)

| Code | When main MUST send |
| --- | --- |
| `HE1` | Enter espresso heat (power-on and re-heat if still cold) |
| `HE0` | Ready criterion met after espresso heat |
| `P11`/`P10` | Start/stop **cold flush** (не еспресо з преінфузією) |
| `P21`/`P20` | Start/stop 2 espresso |
| `HS1`/`HS0` | Steam heat start / steam ready at target |
| `SS1`/`SS0` | Steam flow start/stop **from handle only** |
| `LT1`/`LT0` | Cool path after steam exit via `B1`/`B2` (protocol: LED3 double-blink / off) |
| `DC1`/`DC0` | Decalc reminder latch / clear |

## Feature-specific overrides vs protocol examples

1. У прикладі протоколу повторне `B3` у «пар готовий» може вести до `SS1`.  
   **У цій фічі**: `SS1` лише від відкриття ручки пари; `B3` у `HS0`/`HS1`/`SS*` **ігнорується**.
2. `P11`/`P10` у цій фічі означають **холодний пролив**, не порцію еспресо з преінфузією.
3. `LT1` індикація — **лише** чинний протокол (LED3); одночасне мигання LED1+LED2 **не** вимагається, доки не оновлять панель окремо.

## Service Serial (USB → main)

| Command | Effect |
| --- | --- |
| `DC0` | Clear decalc: send `DC0` to panel, reset EEPROM interval |
| `DC1` (optional test) | Force send `DC1` / latch (стенд) |

Невідомі коди — ігнор (лог у USB).

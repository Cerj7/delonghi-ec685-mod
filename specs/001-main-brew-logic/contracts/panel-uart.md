# Contract: Panel UART (Main Brew Logic consumer)

Джерело істини протоколу: [`docs/button-panel-protocol.md`](../../../docs/button-panel-protocol.md).

Цей файл фіксує **поведінку основної плати** як споживача/виробника кадрів у фічі `001-main-brew-logic` (де вона розходиться з ілюстративним прикладом у протоколі).

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
| `B1` | 1 espresso press | Start/stop brew 1 only from/in brew-1; else ignore in heat/steam/cool |
| `B2` | 2 espresso press | Analogous for brew 2 |
| `B3` | Steam press | Start steam from ready; cancel `HS1`; cancel `HS0` (not `SS1`); ignore during espresso heat/brew |

## Egress (main → panel)

| Code | When main MUST send |
| --- | --- |
| `HE1` | Enter espresso heat (power-on and re-heat after steam cancel if still cold) |
| `HE0` | Ready criterion met after espresso heat |
| `P11`/`P10` | Start/stop 1 espresso |
| `P21`/`P20` | Start/stop 2 espresso |
| `HS1`/`HS0` | Steam heat start / steam ready at target |
| `SS1`/`SS0` | Steam flow start/stop **from handle only** |
| `LT1`/`LT0` | Cool path after steam (or cancel when hotter than espresso setpoint) |
| `DC1`/`DC0` | Decalc reminder latch / clear |

## Feature-specific override vs protocol example

У прикладі протоколу повторне `B3` у стані «пар готовий» може вести до `SS1`.  
**У цій фічі**: `SS1` лише від відкриття ручки пари; `B3` у `HS0` скасовує пар (`LT*` або одразу `LT0`).

## Service Serial (USB → main)

| Command | Effect |
| --- | --- |
| `DC0` | Clear decalc: send `DC0` to panel, reset EEPROM interval |
| `DC1` (optional test) | Force send `DC1` / latch (стенд) |

Невідомі коди — ігнор (лог у USB).

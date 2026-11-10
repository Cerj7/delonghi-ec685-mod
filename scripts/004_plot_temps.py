#!/usr/bin/env python3
"""
Live-графік Tt (термоблок) з Arduino (скетч 004_dual_thermocouple.cpp).

Tw (вода) на графік не йде: оновлюється раз на ~10 с і показується підписом.
Вікно Tt: останні 60 с. Дані можна зберегти в CSV.

Залежності:
  pip install pyserial matplotlib
  # для вікна графіка потрібен GUI-бекенд (один з):
  pip install PyQt5
  # або системний python з tk: sudo apt install python3-tk

Приклад:
  python3 scripts/004_plot_temps.py --port /dev/ttyUSB0
  python3 scripts/004_plot_temps.py --port COM3 --save
  python3 scripts/004_plot_temps.py --port /dev/ttyUSB0 --save --outdir logs
"""

from __future__ import annotations

import argparse
import csv
import math
import sys
from collections import deque
from datetime import datetime
from pathlib import Path

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    print("Потрібен pyserial: pip install pyserial", file=sys.stderr)
    sys.exit(1)

try:
    import matplotlib
except ImportError:
    print("Потрібен matplotlib: pip install matplotlib", file=sys.stderr)
    sys.exit(1)


def _configure_matplotlib_backend() -> str:
    """Обрати інтерактивний бекенд ДО імпорту pyplot, інакше plt.show() нічого не покаже."""
    preferred = ("QtAgg", "Qt5Agg", "TkAgg", "GTK4Agg", "GTK3Agg")
    errors: list[str] = []
    for name in preferred:
        try:
            matplotlib.use(name, force=True)
            return name
        except Exception as exc:  # noqa: BLE001 — перебираємо бекенди
            errors.append(f"{name}: {exc}")

    current = matplotlib.get_backend()
    print(
        "Немає GUI-бекенду для вікна графіка (зараз: "
        f"{current}).\n"
        "У venv поставте, наприклад:\n"
        "  pip install PyQt5\n"
        "або використовуйте системний Python з tkinter:\n"
        "  sudo apt install python3-tk\n"
        "CSV з --save все одно можна писати, але живе вікно не відкриється.",
        file=sys.stderr,
    )
    for line in errors:
        print(f"  - {line}", file=sys.stderr)
    return current


_BACKEND = _configure_matplotlib_backend()

try:
    import matplotlib.pyplot as plt
    from matplotlib.animation import FuncAnimation
    from matplotlib.ticker import MultipleLocator
except ImportError:
    print("Потрібен matplotlib: pip install matplotlib", file=sys.stderr)
    sys.exit(1)


WINDOW_S = 120.0
BAUD = 115200


def list_serial_ports() -> None:
    ports = list(list_ports.comports())
    if not ports:
        print("Серійних портів не знайдено.")
        return
    print("Доступні порти:")
    for p in ports:
        print(f"  {p.device:20s}  {p.description}")


def parse_float(token: str) -> float:
    token = token.strip()
    if token.lower() == "nan" or token == "":
        return float("nan")
    return float(token)


def main() -> int:
    parser = argparse.ArgumentParser(description="Live plot Tt (thermoblock) from EC685 Arduino")
    parser.add_argument("--port", help="Serial port, e.g. /dev/ttyUSB0 or COM3")
    parser.add_argument("--baud", type=int, default=BAUD)
    parser.add_argument("--window", type=float, default=WINDOW_S, help="Seconds of history (default 60)")
    parser.add_argument("--save", action="store_true", help="Append all samples to a CSV file")
    parser.add_argument("--outdir", default="logs", help="Directory for CSV when --save")
    parser.add_argument("--list-ports", action="store_true", help="List serial ports and exit")
    args = parser.parse_args()

    if args.list_ports:
        list_serial_ports()
        return 0

    if not args.port:
        list_serial_ports()
        print("\nВкажіть порт: --port /dev/ttyUSB0", file=sys.stderr)
        return 1

    csv_file = None
    csv_writer = None
    if args.save:
        outdir = Path(args.outdir)
        outdir.mkdir(parents=True, exist_ok=True)
        stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        path = outdir / f"temps_{stamp}.csv"
        csv_file = path.open("w", newline="")
        csv_writer = csv.writer(csv_file)
        csv_writer.writerow(["t_ms", "Tt_C", "Tw_C", "wall_time"])
        print(f"Збереження у {path}")

    ser = serial.Serial(args.port, args.baud, timeout=0.2)
    # Nano після відкриття порту часто ресетиться
    ser.reset_input_buffer()

    t_rel = deque()  # секунди від першої точки Tt
    y_tt = deque()
    t0_ms: float | None = None
    tw_c = float("nan")

    fig, ax = plt.subplots(figsize=(9, 4.5))
    (line_tt,) = ax.plot([], [], color="#c0392b", lw=1.5, label="Tt thermoblock")
    ax.set_ylabel("Tt °C")
    ax.set_xlabel("час, с (вікно {:.0f} с)".format(args.window))
    ax.legend(loc="upper right")
    # Підписи кожні 5 °C (…75, 80, 85…); пунктирна сітка на кожен градус.
    ax.yaxis.set_major_locator(MultipleLocator(5))
    ax.yaxis.set_minor_locator(MultipleLocator(1))
    ax.grid(True, which="major", axis="y", alpha=0.45)
    ax.grid(True, which="minor", axis="y", linestyle="--", alpha=0.35)
    ax.grid(True, which="major", axis="x", alpha=0.3)
    tw_text = ax.text(
        0.01,
        0.97,
        "Tw = —",
        transform=ax.transAxes,
        va="top",
        ha="left",
        fontsize=11,
        color="#2980b9",
    )
    fig.suptitle(f"EC685 Tt — {args.port}")
    fig.tight_layout()

    def ingest() -> None:
        nonlocal t0_ms, tw_c
        while ser.in_waiting:
            raw = ser.readline().decode("utf-8", errors="ignore").strip()
            if not raw:
                continue

            # # Tw,<t_ms>,<Tw_C>
            if raw.startswith("#"):
                body = raw[1:].strip()
                if body.lower().startswith("tw"):
                    parts = body.split(",")
                    if len(parts) >= 3:
                        try:
                            tw_c = parse_float(parts[2])
                        except ValueError:
                            pass
                continue

            parts = raw.split(",")
            if len(parts) < 2:
                continue
            try:
                t_ms = float(parts[0])
                tt = parse_float(parts[1])
            except ValueError:
                continue

            if t0_ms is None:
                t0_ms = t_ms
            rel = (t_ms - t0_ms) / 1000.0
            t_rel.append(rel)
            y_tt.append(tt)

            cutoff = rel - args.window
            while t_rel and t_rel[0] < cutoff:
                t_rel.popleft()
                y_tt.popleft()

            if csv_writer is not None:
                tw_out = "" if math.isnan(tw_c) else tw_c
                tt_out = "" if math.isnan(tt) else tt
                csv_writer.writerow(
                    [int(t_ms), tt_out, tw_out, datetime.now().isoformat(timespec="milliseconds")]
                )

    def on_frame(_frame: int):
        ingest()
        if math.isnan(tw_c):
            tw_text.set_text("Tw = —")
        else:
            tw_text.set_text(f"Tw = {tw_c:.1f} °C")

        if not t_rel:
            return (line_tt,)

        xs = list(t_rel)
        line_tt.set_data(xs, list(y_tt))

        xmin = max(0.0, xs[-1] - args.window)
        xmax = max(args.window, xs[-1])
        ax.set_xlim(xmin, xmax)

        vals = [v for v in y_tt if v == v]
        if vals:
            pad = max(1.0, (max(vals) - min(vals)) * 0.1)
            # Цілі межі, щоб горизонтальні лінії сітки збігалися з цілими °C.
            y_lo = math.floor(min(vals) - pad)
            y_hi = math.ceil(max(vals) + pad)
            if y_hi <= y_lo:
                y_hi = y_lo + 1
            ax.set_ylim(y_lo, y_hi)

        return (line_tt,)

    # Тримати посилання на anim до кінця plt.show(), інакше GC знесе анімацію.
    anim = FuncAnimation(fig, on_frame, interval=200, blit=False, cache_frame_data=False)

    backend = matplotlib.get_backend().lower()
    if backend == "agg" or not hasattr(fig.canvas, "manager"):
        print(
            f"Бекенд '{matplotlib.get_backend()}' неінтерактивний — вікно не відкриється.\n"
            "Поставте GUI: pip install PyQt5  (або python3-tk для системного Python).",
            file=sys.stderr,
        )
        ser.close()
        if csv_file is not None:
            csv_file.close()
        return 1

    print(f"Графік: бекенд {_BACKEND}. Закрийте вікно, щоб вийти.")
    try:
        plt.show()
    finally:
        del anim
        ser.close()
        if csv_file is not None:
            csv_file.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())

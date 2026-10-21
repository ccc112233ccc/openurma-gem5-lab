#!/usr/bin/env python3
"""Render the two-host READ comparison to standalone PNG charts.

Requires Pillow; the Codex workspace Python runtime provides it.
"""

from __future__ import annotations

import csv
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
EVIDENCE = ROOT / "experiments/read-compare-20261021"
OUTPUT = ROOT / "results/read-compare-20261021"
FONT = "/System/Library/Fonts/Hiragino Sans GB.ttc"
COLORS = {"physical": "#182B49", "gem5": "#2276C5", "qemu": "#DF714B"}


def load(path: Path, column: str) -> list[tuple[int, float]]:
    with path.open(newline="") as stream:
        return sorted((int(row["size_bytes"]), float(row[column]))
                      for row in csv.DictReader(stream))


def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(FONT, size)


def label(size: int) -> str:
    if size >= 1024 * 1024:
        return f"{size // (1024 * 1024)}M"
    if size >= 1024:
        return f"{size // 1024}K"
    return str(size)


def marker(draw: ImageDraw.ImageDraw, key: str, x: int, y: int) -> None:
    color = COLORS[key]
    if key == "physical":
        draw.ellipse((x - 6, y - 6, x + 6, y + 6), fill=color,
                     outline="white", width=2)
    elif key == "gem5":
        draw.rectangle((x - 7, y - 7, x + 7, y + 7), fill="white",
                       outline=color, width=4)
    else:
        draw.polygon(((x, y - 8), (x + 8, y), (x, y + 8), (x - 8, y)),
                     fill="white", outline=color, width=3)


def chart(title: str, unit: str, curves: dict[str, list[tuple[int, float]]],
          y_ticks: list[float], footnote: str, output: Path,
          xmax: int = 10485760,
          annotations: list[dict] | None = None,
          transition: bool = False) -> None:
    width, height = 1800, 1040
    image = Image.new("RGB", (width, height), "#F5F7FB")
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((35, 30, width - 35, height - 30), radius=30, fill="white")
    draw.text((105, 72), title, font=font(49), fill="#172A46")
    draw.text((105, 142), "双节点 · 单 Jetty · 单端口 400 Gb/s · CTP READ · 375 ns + 500 ns + 375 ns",
              font=font(24), fill="#65758C")
    x0, y0, x1, y1 = 160, 250, 1690, 795
    xmin = 2
    ymin, ymax = y_ticks[0], y_ticks[-1]
    xlog0, xlog1 = math.log10(xmin), math.log10(xmax)
    ylog0, ylog1 = math.log10(ymin), math.log10(ymax)

    def xy(x: float, y: float) -> tuple[float, float]:
        px = x0 + (math.log10(x) - xlog0) / (xlog1 - xlog0) * (x1 - x0)
        py = y1 - (math.log10(y) - ylog0) / (ylog1 - ylog0) * (y1 - y0)
        return px, py

    x_ticks = ([2, 16, 128, 1024, 8192, 65536, 524288, 4194304, 10485760]
               if xmax > 1048576 else
               [2, 16, 128, 1024, 8192, 65536, 524288, 1048576])
    for tick in x_ticks:
        px, _ = xy(tick, ymin)
        draw.line((px, y0, px, y1), fill="#E8EDF4", width=2)
        text = label(tick)
        box = draw.textbbox((0, 0), text, font=font(23))
        draw.text((px - (box[2] - box[0]) / 2, y1 + 15), text,
                  font=font(23), fill="#56667D")
    for tick in y_ticks:
        _, py = xy(xmin, tick)
        draw.line((x0, py, x1, py), fill="#E8EDF4", width=2)
        text = f"{tick:g}"
        box = draw.textbbox((0, 0), text, font=font(22))
        draw.text((x0 - 22 - box[2] + box[0], py - 14), text,
                  font=font(22), fill="#56667D")
    draw.line((x0, y0, x0, y1, x1, y1), fill="#9CACBE", width=3)
    if transition:
        boundary, _ = xy(4096, ymin)
        for top in range(y0 + 4, y1, 18):
            draw.line((boundary, top, boundary, min(top + 9, y1)),
                      fill="#A8B5C5", width=2)
        draw.text((boundary + 12, y0 + 9), "4 KiB 转折", font=font(19),
                  fill="#687A90")
    draw.text((x0, 208), unit + "  ·  对数刻度", font=font(24), fill="#56667D")
    draw.text((x1 - 175, y1 + 62), "消息大小 / B", font=font(23), fill="#56667D")

    names = {"physical": "实机", "gem5": "gem5", "qemu": "QEMU"}
    for i, key in enumerate(("physical", "gem5", "qemu")):
        points = [(int(round(px)), int(round(py))) for size, value in curves[key]
                  if size <= xmax
                  for px, py in [xy(size, value)] if value > 0]
        draw.line(points, fill=COLORS[key], width=5, joint="curve")
        for px, py in points:
            marker(draw, key, px, py)
        lx = 110 + i * 310
        ly = 890
        draw.line((lx, ly, lx + 75, ly), fill=COLORS[key], width=6)
        marker(draw, key, lx + 39, ly)
        draw.text((lx + 92, ly - 18), names[key], font=font(28), fill="#273A54")
    for annotation in annotations or []:
        key, size, value = annotation["anchor"]
        ax, ay = xy(size, value)
        bx, by, bw, bh = annotation["box"]
        color = COLORS[key]
        draw.line((ax, ay, bx + bw // 2, by + bh // 2), fill=color, width=2)
        draw.ellipse((ax - 5, ay - 5, ax + 5, ay + 5), fill=color)
        draw.rounded_rectangle((bx, by, bx + bw, by + bh), radius=12,
                               fill="white", outline="#C9D5E3", width=2)
        draw.rectangle((bx + 1, by + 1, bx + 7, by + bh - 1), fill=color)
        draw.text((bx + 21, by + 13), annotation["heading"],
                  font=font(23), fill="#243850")
        for index, line in enumerate(annotation["lines"]):
            draw.text((bx + 21, by + 51 + index * 30), line,
                      font=font(20), fill="#52647A")
    for line_number, line in enumerate(footnote.split("\n")):
        draw.text((110, 935 + line_number * 31), line,
                  font=font(20), fill="#687A90")
    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output, optimize=True)


def main() -> None:
    physical_bw = load(ROOT / "results/physical-read-bw-20261009.csv", "average_mib_s")
    gem5_bw = load(ROOT / "results/gem5-macos-64k-read-curve-20261020.csv", "average_mib_s")
    qemu_bw = load(EVIDENCE / "qemu-scan/read-bw.csv", "average_mib_s")
    physical_rate = load(ROOT / "results/physical-read-bw-20261009.csv", "message_rate_mpps")
    gem5_rate = load(ROOT / "results/gem5-macos-64k-read-curve-20261020.csv", "message_rate_mpps")
    qemu_rate = load(EVIDENCE / "qemu-scan/read-bw.csv", "message_rate_mpps")
    physical_lat = load(ROOT / "results/physical-read-lat-20261009.csv", "average_us")
    gem5_lat = load(EVIDENCE / "gem5-scan/read-lat.csv", "average_us")
    qemu_lat = load(EVIDENCE / "qemu-scan/read-lat.csv", "average_us")
    chart("(a) READ 带宽 · 消息大小扫描", "平均带宽 / MiB/s",
          {"physical": physical_bw, "gem5": gem5_bw, "qemu": qemu_bw},
          [0.5, 1, 10, 100, 1000, 10000, 100000],
          "gem5：2026-10-20 稳态全曲线；QEMU：单核 Cortex-A72 / TCG 同步计时，六点短扫。\n"
          "实机小包 n=16,384；QEMU 小包 n=4,096，大包缩减操作数。大包只比较平台趋势。",
          OUTPUT / "read-bandwidth.png",
          transition=True,
          annotations=[{"anchor": ("physical", 4096, 45172.39),
                        "box": (1060, 520, 570, 122),
                        "heading": "4 KiB 后接近 400 Gb/s 端口上限",
                        "lines": ["实机 45,172 · gem5 46,512 MiB/s",
                                  "QEMU 1,862 MiB/s；大包逐渐追平"]}])
    chart("(b) READ 包率 · 消息大小扫描", "包率 / Mpps",
          {"physical": physical_rate, "gem5": gem5_rate, "qemu": qemu_rate},
          [0.003, 0.01, 0.1, 1, 10, 30],
          "包率使用各点原始 MsgRate 字段；gem5 为 26 点稳态曲线，QEMU 为六点短扫。"
          "连线仅辅助阅读，未测点无插值数据。\n"
          "实机 / gem5 小包 n=16,384；QEMU 小包 n=4,096。"
          "数据来源：用户实测、2026-10-20 gem5、2026-10-21 QEMU。",
          OUTPUT / "read-message-rate.png",
          transition=True,
          annotations=[{"anchor": ("physical", 128, 23.098464),
                        "box": (245, 575, 700, 122),
                        "heading": "128 B：小包包率平台",
                        "lines": ["实机 23.10 · gem5 21.46 Mpps（实机的 92.9%）",
                                  "QEMU 0.477 Mpps（单核 TCG）"]},
                       {"anchor": ("physical", 1048576, 0.045609),
                        "box": (1125, 315, 505, 92),
                        "heading": "1 MiB：链路主导",
                        "lines": ["三者约 0.046 Mpps"]}])
    chart("(c) READ 平均延迟 · 消息大小扫描", "平均延迟 / us",
          {"physical": physical_lat, "gem5": gem5_lat, "qemu": qemu_lat},
          [2, 5, 10, 20, 50, 100, 250],
          "实机：每点 16,384 次；gem5 / QEMU：每点 256 次。只比较均值趋势，不比较尾部百分位。\n"
          "QEMU 为单核 Cortex-A72 / TCG；gem5 为 3.2 GHz ArmO3。CPU 模型不同。",
          OUTPUT / "read-latency.png", xmax=1048576,
          annotations=[{"anchor": ("physical", 128, 3.38),
                        "box": (260, 330, 635, 122),
                        "heading": "128 B：固定往返开销占主导",
                        "lines": ["实机 3.38 · gem5 3.46 us",
                                  "QEMU 8.58 us（CPU 模型不同）"]},
                       {"anchor": ("physical", 1048576, 25.64),
                        "box": (1040, 330, 600, 122),
                        "heading": "1 MiB：传输时间显著增加",
                        "lines": ["实机 25.64 · gem5 24.86 us",
                                  "QEMU 29.81 us"]}])


if __name__ == "__main__":
    main()

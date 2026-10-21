#!/usr/bin/env python3
"""Plot matched QEMU READ scans with all UB synchronization on/off."""

from __future__ import annotations

import csv
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "experiments/read-compare-20261021"
OUTPUT = ROOT / "results/qemu-sync-compare-20261021/qemu-sync-ablation.png"
FONT = "/System/Library/Fonts/Hiragino Sans GB.ttc"
COLORS = {"on": "#256DB3", "off1us": "#299274", "off1ms": "#D96A43"}
SIZES = (2, 128, 1024, 4096, 65536, 1048576)


def font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(FONT, size)


def load(directory: Path, kind: str, field: str) -> dict[int, float]:
    with (directory / f"read-{kind}.csv").open(newline="") as stream:
        return {int(row["size_bytes"]): float(row[field])
                for row in csv.DictReader(stream)}


def panel(draw: ImageDraw.ImageDraw, y0: int, title: str, unit: str,
          ticks: tuple[float, ...], curves: dict[str, dict[int, float]],
          annotation: str) -> None:
    x0, x1, y1 = 165, 1640, y0 + 280
    lo_x, hi_x = math.log10(SIZES[0]), math.log10(SIZES[-1])
    lo_y, hi_y = math.log10(ticks[0]), math.log10(ticks[-1])

    def pos(size: int, value: float) -> tuple[int, int]:
        return (round(x0 + (math.log10(size) - lo_x) / (hi_x - lo_x) * (x1 - x0)),
                round(y1 - (math.log10(value) - lo_y) / (hi_y - lo_y) * (y1 - y0)))

    draw.text((165, y0 - 60), title, font=font(29), fill="#172B49")
    draw.text((1210, y0 - 55), annotation, font=font(20), fill="#536781")
    for tick in ticks:
        _, y = pos(SIZES[0], tick)
        draw.line((x0, y, x1, y), fill="#E5ECF3", width=2)
        label = f"{tick:g}"
        width = draw.textbbox((0, 0), label, font=font(19))[2]
        draw.text((x0 - width - 17, y - 12), label,
                  font=font(19), fill="#61718A")
    for size in SIZES:
        x, _ = pos(size, ticks[0])
        draw.line((x, y0, x, y1), fill="#EEF2F7", width=2)
        label = ("1M" if size == 1048576 else "64K" if size == 65536
                 else "4K" if size == 4096 else "1K" if size == 1024
                 else str(size))
        width = draw.textbbox((0, 0), label, font=font(19))[2]
        draw.text((x - width / 2, y1 + 10), label,
                  font=font(19), fill="#61718A")
    draw.line((x0, y0, x0, y1, x1, y1), fill="#9CAFC2", width=3)
    draw.text((x0 + 7, y0 + 5), unit, font=font(18), fill="#687A90")
    for key in ("on", "off1us", "off1ms"):
        data = curves[key]
        points = [pos(size, data[size]) for size in SIZES if data[size] > 0]
        draw.line(points, fill=COLORS[key], width=5, joint="curve")
        for x, y in points:
            if key == "on":
                draw.ellipse((x - 6, y - 6, x + 6, y + 6),
                             fill="white", outline=COLORS[key], width=4)
            elif key == "off1us":
                draw.rectangle((x - 6, y - 6, x + 6, y + 6),
                               fill="white", outline=COLORS[key], width=4)
            else:
                draw.polygon(((x, y - 7), (x + 7, y), (x, y + 7), (x - 7, y)),
                             fill="white", outline=COLORS[key], width=3)


def main() -> None:
    sync = BASE / "qemu-scan"
    off1ms = BASE / "qemu-all-sync-off-scan"
    off1us = BASE / "qemu-all-sync-off-poll1us-scan"
    width, height = 1800, 1600
    image = Image.new("RGB", (width, height), "#F4F7FB")
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((38, 28, 1762, 1570), radius=24, fill="white")
    draw.text((105, 65), "QEMU READ：关闭全部 UB 同步后的曲线", font=font(43), fill="#142944")
    draw.text((105, 126), "双节点 · 单 Jetty · 单端口 400 Gb/s · Cortex-A72 TCG icount shift=0 · 同一负载规模",
              font=font(23), fill="#64758C")
    for index, (key, label) in enumerate((
        ("on", "同步开启"),
        ("off1us", "全部关闭 · 1 us 轮询"),
        ("off1ms", "全部关闭 · 1 ms 轮询"),
    )):
        x = 115 + index * 525
        draw.line((x, 198, x + 80, 198), fill=COLORS[key], width=6)
        if key == "on":
            draw.ellipse((x + 34, 192, x + 46, 204), fill="white",
                         outline=COLORS[key], width=3)
        elif key == "off1us":
            draw.rectangle((x + 34, 192, x + 46, 204), fill="white",
                           outline=COLORS[key], width=3)
        else:
            draw.polygon(((x + 40, 191), (x + 47, 198),
                          (x + 40, 205), (x + 33, 198)),
                         fill="white", outline=COLORS[key], width=3)
        draw.text((x + 100, 179), label, font=font(27), fill="#263A56")
    panels = (
        (280, "(a) 平均带宽", "MiB/s · 对数刻度",
         (0.001, 0.01, 0.1, 1, 10, 100, 1000, 10000, 100000),
         "bw", "average_mib_s", "128 B：58.19 / 7.35 / 0.13"),
        (685, "(b) 包率", "Mpps · 对数刻度",
         (0.0001, 0.001, 0.01, 0.1, 1),
         "bw", "message_rate_mpps", "128 B：0.477 / 0.060 / 0.001"),
        (1090, "(c) 平均延迟", "us · 对数刻度",
         (1, 10, 100, 1000, 10000),
         "lat", "average_us", "128 B：8.58 / 83.41 / 3109"),
    )
    for y, title, unit, ticks, kind, field, note in panels:
        panel(draw, y, title, unit, ticks,
              {"on": load(sync, kind, field),
               "off1us": load(off1us, kind, field),
               "off1ms": load(off1ms, kind, field)}, note)
    draw.text((105, 1500), "每点：延迟 256 次；小包带宽 4096 次。2 B 在 1 ms 轮询下平均带宽四舍五入为 0.00，故未在对数图中绘点。",
              font=font(20), fill="#677991")
    draw.text((105, 1530), "无同步数据仅描述此运行的 guest 计时结果；跨进程虚拟时间失去共同前沿，不能作为硬件性能预测。",
              font=font(20), fill="#A14E3B")
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    image.save(OUTPUT, optimize=True)


if __name__ == "__main__":
    main()

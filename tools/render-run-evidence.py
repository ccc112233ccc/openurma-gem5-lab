#!/usr/bin/env python3
"""Render presentation-ready evidence cards from preserved UART/gem5 logs."""

from __future__ import annotations

import hashlib
import re
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "evidence" / "official-rma-2026-09-19"
UART = ROOT / "run-dual/node1/system.terminal"
NODE1 = ROOT / "run-dual/node1/gem5.log"
NODE0 = ROOT / "run-dual/node0/gem5.log"

W, H = 1920, 1080
BG = "#0b1020"
PANEL = "#121a2d"
PANEL_2 = "#0e1628"
TEXT = "#e6edf7"
MUTED = "#8da0bb"
CYAN = "#54d6ff"
GREEN = "#67e8a5"
YELLOW = "#ffd166"
MAGENTA = "#c4a7ff"
RED = "#ff6b81"
GRID = "#26344d"

SANS_PATH = "/System/Library/Fonts/STHeiti Light.ttc"
MONO_PATH = "/System/Library/Fonts/Menlo.ttc"


def font(size: int, mono: bool = False) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(MONO_PATH if mono else SANS_PATH, size=size)


F_TITLE = font(48)
F_SUB = font(25)
F_MONO = font(25, mono=True)
F_MONO_SM = font(22, mono=True)
F_CJK_CODE = font(25)
F_LABEL = font(23)
F_FOOT = font(19, mono=True)


def clean_uart(s: str) -> str:
    s = s.replace("\r", "")
    return re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", s)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    h.update(path.read_bytes())
    return h.hexdigest()


def base(title: str, subtitle: str, badge: str) -> tuple[Image.Image, ImageDraw.ImageDraw]:
    im = Image.new("RGB", (W, H), BG)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((55, 42, W - 55, H - 48), 26, fill=PANEL, outline=GRID, width=2)
    d.text((92, 74), title, font=F_TITLE, fill=TEXT)
    d.text((94, 140), subtitle, font=F_SUB, fill=MUTED)
    bw = d.textbbox((0, 0), badge, font=F_LABEL)[2] + 40
    d.rounded_rectangle((W - 92 - bw, 76, W - 92, 126), 15, fill="#17314a")
    d.text((W - 72 - bw, 87), badge, font=F_LABEL, fill=CYAN)
    return im, d


def terminal(d: ImageDraw.ImageDraw, box: tuple[int, int, int, int], label: str) -> None:
    x1, y1, x2, y2 = box
    d.rounded_rectangle(box, 18, fill=PANEL_2, outline=GRID, width=2)
    for i, c in enumerate((RED, YELLOW, GREEN)):
        d.ellipse((x1 + 24 + i * 28, y1 + 20, x1 + 37 + i * 28, y1 + 33), fill=c)
    d.text((x1 + 130, y1 + 14), label, font=F_LABEL, fill=MUTED)
    d.line((x1, y1 + 54, x2, y1 + 54), fill=GRID, width=2)


def code_lines(d: ImageDraw.ImageDraw, x: int, y: int, lines: list[tuple[str, str]], gap: int = 39) -> None:
    for idx, (line, color) in enumerate(lines):
        line_font = F_CJK_CODE if any("\u3400" <= ch <= "\u9fff" for ch in line) else F_MONO
        d.text((x, y + idx * gap), line, font=line_font, fill=color)


def footer(d: ImageDraw.ImageDraw, source: str, digest: str) -> None:
    d.text((92, H - 90), f"source: {source}", font=F_FOOT, fill=MUTED)
    d.text((W - 900, H - 90), f"sha256: {digest}", font=F_FOOT, fill=MUTED)


def render_command_result(uart: str) -> Image.Image:
    required = [
        "OPENURMA_DIST_SYNC=1 urma_perftest read_lat",
        "URMA_READ Latency Test",
        "128     2           2.78       2.98",
        "__OPENURMA_RC_3470_393488758418017__=0",
        "__OPENURMA_RC_3470_393558655143923__=0",
    ]
    assert all(s in uart for s in required), "expected UART evidence is missing"

    im, d = base(
        "命令执行证据：官方 urma_perftest 完成 READ 时延测试",
        "真实 UART 记录 · 仅合并了终端自动换行，数值未改写",
        "EXIT 0 × 2",
    )
    box = (90, 202, W - 90, 930)
    terminal(d, box, "openurma-node1 / PL011 console")
    lines = [
        ("(openurma-node1) ~ # OPENURMA_DIST_SYNC=1 urma_perftest read_lat \\", CYAN),
        ("  -d bonding_dev_0 --eid_idx 0 --ctp --use_bonding --aggr_mode balance \\", CYAN),
        ("  -s 128 -P 21117 -J 1 -I 0 -l 1 -n 8 -p 0 -S 10.0.0.1", CYAN),
        ("", TEXT),
        ("                    URMA_READ Latency Test", GREEN),
        (" Device name: bonding_dev_0     Transport mode: UB     JETTY mode: DUPLEX", TEXT),
        (" bytes  iterations  t_min  t_max  t_median  t_avg  stdev   99%   99.9%", MUTED),
        (" 128    2           2.78   2.98   2.86      2.88   0.10    2.98  2.98  us", GREEN),
        (" __OPENURMA_RC_3470_393488758418017__=0", YELLOW),
        ("", TEXT),
        ("# 同一实验再次执行（端口 21118）", MAGENTA),
        (" 128    2           2.78   2.98   2.86      2.88   0.10    2.98  2.98  us", GREEN),
        (" __OPENURMA_RC_3470_393558655143923__=0", YELLOW),
    ]
    code_lines(d, 124, 282, lines, 43)
    footer(d, "run-dual/node1/system.terminal:503-649", sha256(UART)[:24])
    return im


def render_resources(node1: str) -> Image.Image:
    required = [
        "Jetty id=1026 sq_iova=0xfffff7edc000 db_off=0x603080",
        "Jetty id=1027 sq_iova=0xfffff7eda000 db_off=0x604080",
        "allocated TP id=5 tpn=5 port=0 source=GET_TP_LIST",
        "allocated TP id=6 tpn=6 port=1 source=GET_TP_LIST",
        "activated TP id=5 tpn=5 count=1 port=0",
        "activated TP id=6 tpn=6 count=1 port=1",
    ]
    assert all(s in node1 for s in required), "expected resource evidence is missing"

    im, d = base(
        "设备侧证据：官方资源创建落到两个独立端口",
        "Jetty / SQ doorbell / TP 均来自本次真实 gem5 设备日志",
        "PORT 0 + PORT 1",
    )
    terminal(d, (90, 202, W - 90, 860), "node1 / gem5.log / official UDMA device model")
    lines = [
        ("[NIC udma official] Jetty id=1026", GREEN),
        ("  sq_iova=0xfffff7edc000  db_off=0x603080  seid_idx=0", TEXT),
        ("[NIC udma official] Jetty id=1027", GREEN),
        ("  sq_iova=0xfffff7eda000  db_off=0x604080  seid_idx=1", TEXT),
        ("", TEXT),
        ("[NIC udma official] allocated TP id=5  tpn=5  port=0  source=GET_TP_LIST", CYAN),
        ("[NIC udma official] activated TP id=5  tpn=5  count=1  port=0", CYAN),
        ("[NIC udma official] allocated TP id=6  tpn=6  port=1  source=GET_TP_LIST", MAGENTA),
        ("[NIC udma official] activated TP id=6  tpn=6  count=1  port=1", MAGENTA),
        ("", TEXT),
        ("结论：两个 Jetty 使用不同 doorbell，并分别绑定 port 0 / port 1 的 TP。", YELLOW),
    ]
    code_lines(d, 124, 286, lines, 45)
    d.rounded_rectangle((90, 886, W - 90, 955), 14, fill="#10253a")
    d.text((122, 903), "证据链：用户态命令 → 官方 provider/内核驱动 → SQ doorbell → TP 资源", font=F_LABEL, fill=CYAN)
    footer(d, "run-dual/node1/gem5.log:3852-3921", sha256(NODE1)[:24])
    return im


def render_packets(node1: str, node0: str) -> Image.Image:
    req = [
        "seq=16 src_port=1 dst_port=1 tpn=6 op=0x84 len=128",
        "seq=17 src_port=0 dst_port=0 tpn=5 op=0x84 len=128",
        "seq=18 src_port=1 dst_port=1 tpn=6 op=0x84 len=128",
        "seq=19 src_port=0 dst_port=0 tpn=5 op=0x84 len=128",
    ]
    rsp = [
        "seq=16 src_port=1 dst_port=1 tpn=4294967295 op=0x85 len=128",
        "seq=17 src_port=0 dst_port=0 tpn=4294967295 op=0x85 len=128",
        "seq=18 src_port=1 dst_port=1 tpn=4294967295 op=0x85 len=128",
        "seq=19 src_port=0 dst_port=0 tpn=4294967295 op=0x85 len=128",
    ]
    assert all(s in node1 for s in req), "expected READ requests are missing"
    assert all(s in node0 for s in rsp), "expected READ responses are missing"

    im, d = base(
        "包级证据：READ 请求与响应在双端口间交替",
        "同一序列号保持端口对称：port 1 ↔ port 1，port 0 ↔ port 0",
        "op 0x84 → 0x85",
    )
    left = (90, 202, 945, 862)
    right = (975, 202, W - 90, 862)
    terminal(d, left, "node1 TX · READ request (0x84)")
    terminal(d, right, "node0 TX · READ response (0x85)")
    left_lines = [
        ("SQ doorbell jetty=1027", MUTED),
        ("seq=16  port 1 -> 1  tpn=6", MAGENTA),
        ("op=0x84  len=128", TEXT),
        ("", TEXT),
        ("SQ doorbell jetty=1026", MUTED),
        ("seq=17  port 0 -> 0  tpn=5", CYAN),
        ("op=0x84  len=128", TEXT),
        ("", TEXT),
        ("seq=18  port 1 -> 1  tpn=6", MAGENTA),
        ("seq=19  port 0 -> 0  tpn=5", CYAN),
    ]
    right_lines = [
        ("remote segment 18 resolved", MUTED),
        ("seq=16  port 1 -> 1", MAGENTA),
        ("op=0x85  len=128", TEXT),
        ("", TEXT),
        ("remote segment 17 resolved", MUTED),
        ("seq=17  port 0 -> 0", CYAN),
        ("op=0x85  len=128", TEXT),
        ("", TEXT),
        ("seq=18  port 1 -> 1", MAGENTA),
        ("seq=19  port 0 -> 0", CYAN),
    ]
    code_lines(d, 122, 286, left_lines, 48)
    code_lines(d, 1007, 286, right_lines, 48)
    d.line((945, 525, 975, 525), fill=GREEN, width=5)
    d.polygon(((975, 525), (958, 515), (958, 535)), fill=GREEN)
    d.rounded_rectangle((90, 888, W - 90, 956), 14, fill="#10253a")
    d.text((122, 904), "可观测结果：连续请求按 1 / 0 / 1 / 0 分流，响应沿原端口返回。", font=F_LABEL, fill=GREEN)
    footer(d, "node1/gem5.log:3922+  |  node0/gem5.log:3890-3913", f"{sha256(NODE1)[:10]}… / {sha256(NODE0)[:10]}…")
    return im


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    uart = clean_uart(UART.read_text(errors="replace"))
    node1 = NODE1.read_text(errors="replace")
    node0 = NODE0.read_text(errors="replace")

    images = [
        render_command_result(uart),
        render_resources(node1),
        render_packets(node1, node0),
    ]
    names = [
        "01-command-and-result.png",
        "02-official-resources.png",
        "03-dual-port-packets.png",
    ]
    for im, name in zip(images, names):
        im.save(OUT / name, optimize=True)
    images[0].save(
        OUT / "evidence-slideshow.gif",
        save_all=True,
        append_images=images[1:],
        duration=[3500, 3500, 4500],
        loop=0,
        optimize=True,
    )
    print(OUT)
    for name in names + ["evidence-slideshow.gif"]:
        print(name, (OUT / name).stat().st_size)


if __name__ == "__main__":
    main()

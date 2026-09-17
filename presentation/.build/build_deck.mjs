import fs from "node:fs/promises";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { Presentation, PresentationFile } from "@oai/artifact-tool";
import sharp from "sharp";

const ROOT = "/Users/caobo/workspace";
const LAB = path.join(ROOT, "openurma-gem5-lab");
const OPENURMA = path.join(ROOT, "OpenURMA");
const PRESENTATION_DIR = path.join(LAB, "presentation");
const BUILD_DIR = path.join(PRESENTATION_DIR, ".build");
const OUTPUT_DIR = path.join(PRESENTATION_DIR, "output");
const SKILL_DIR = "/Users/caobo/.codex/plugins/cache/openai-primary-runtime/presentations/26.905.11957/skills/presentations";
const RUNTIME_NODE = "/Users/caobo/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node";
const RUNTIME_NODE_MODULES = "/Users/caobo/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules";
const RUNTIME_PYTHON = "/Users/caobo/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3";
const VERSION = process.env.DECK_VERSION || "v1";
const FINAL_PPTX = path.join(OUTPUT_DIR, "OpenURMA_gem5_dual_node_overview_" + VERSION + ".pptx");

const IMG_BOOT = "/var/folders/yp/xpx6mk9x5xzgmpf95djdjwwc0000gn/T/codex-clipboard-d4ec0f4e-e2a8-4875-a56f-49109133a2d3.png";
const IMG_ACTIVE = "/var/folders/yp/xpx6mk9x5xzgmpf95djdjwwc0000gn/T/codex-clipboard-f7af11e0-5108-4c5d-b56e-c98f871328f4.png";
const IMG_RESULT = "/var/folders/yp/xpx6mk9x5xzgmpf95djdjwwc0000gn/T/codex-clipboard-e0a2b538-17e5-456c-815e-2728573719f7.png";

const FONT = "Hiragino Sans GB";
const MONO = "FiraCode Nerd Font Mono";
const C = {
  bg: "#F5F7FA",
  paper: "#FFFFFF",
  navy: "#0B1526",
  navy2: "#15243B",
  teal: "#149D93",
  tealDark: "#08786F",
  tealPale: "#E7F6F4",
  coral: "#E95B5B",
  coralPale: "#FCEBEC",
  amber: "#D99822",
  amberPale: "#FFF5DE",
  blue: "#3979C6",
  bluePale: "#EAF2FC",
  ink: "#172235",
  slate: "#526174",
  light: "#D9E0E8",
  lighter: "#E9EEF3",
  code: "#111827",
  codeText: "#E5EDF8",
  muted: "#93A1B3",
};

const { makeNativeBulletParagraphs, finalizePresentation } = await import(
  pathToFileURL(path.join(SKILL_DIR, "container_tools/artifact_tool_utils.mjs")).href
);

process.env.RUNTIME_NODE = RUNTIME_NODE;
process.env.RUNTIME_NODE_MODULES = RUNTIME_NODE_MODULES;

await fs.mkdir(BUILD_DIR, { recursive: true });
await fs.mkdir(OUTPUT_DIR, { recursive: true });
const [bootSource, activeSource, resultBytes] = await Promise.all([
  fs.readFile(IMG_BOOT),
  fs.readFile(IMG_ACTIVE),
  fs.readFile(IMG_RESULT),
]);
const [bootBytes, activeBytes] = await Promise.all([
  sharp(bootSource).extract({ left: 0, top: 430, width: 2174, height: 620 }).png().toBuffer(),
  sharp(activeSource).extract({ left: 0, top: 0, width: 1640, height: 160 }).png().toBuffer(),
]);

const deck = Presentation.create({ slideSize: { width: 1280, height: 720 } });

function shape(slide, geometry, position, fill = "none", lineFill = "none", lineWidth = 0, name) {
  return slide.shapes.add({
    geometry,
    ...(name ? { name } : {}),
    position,
    fill,
    line: { style: "solid", fill: lineFill, width: lineWidth },
  });
}

function textBox(slide, text, position, opts = {}) {
  const s = shape(slide, "textbox", position, opts.fill || "none", opts.lineFill || "none", opts.lineWidth || 0, opts.name);
  s.text = text;
  s.text.style = {
    typeface: opts.typeface || FONT,
    fontSize: opts.fontSize || 24,
    bold: opts.bold || false,
    italic: opts.italic || false,
    color: opts.color || C.ink,
    alignment: opts.alignment || "left",
    verticalAlignment: opts.verticalAlignment || "top",
    autoFit: opts.autoFit || "none",
    wrap: opts.wrap || "square",
    insets: opts.insets || { top: 0, right: 0, bottom: 0, left: 0 },
  };
  return s;
}

function richTextBox(slide, paragraphs, position, opts = {}) {
  const s = shape(slide, "textbox", position, opts.fill || "none", opts.lineFill || "none", opts.lineWidth || 0, opts.name);
  s.text = paragraphs;
  s.text.style = {
    typeface: opts.typeface || FONT,
    fontSize: opts.fontSize || 24,
    color: opts.color || C.ink,
    alignment: opts.alignment || "left",
    verticalAlignment: opts.verticalAlignment || "top",
    autoFit: opts.autoFit || "none",
    wrap: "square",
    insets: opts.insets || { top: 0, right: 0, bottom: 0, left: 0 },
  };
  return s;
}

function roundRect(slide, position, fill, lineFill = fill, radius = 18, name) {
  const s = shape(slide, "roundRect", position, fill, lineFill, lineFill === fill ? 0 : 1, name);
  s.borderRadius = radius;
  return s;
}

function pill(slide, label, position, fill, color, opts = {}) {
  const p = roundRect(slide, position, fill, opts.lineFill || fill, position.height / 2);
  p.text = label;
  p.text.style = {
    typeface: opts.typeface || FONT,
    fontSize: opts.fontSize || 16,
    bold: opts.bold === undefined ? true : opts.bold,
    color,
    alignment: "center",
    verticalAlignment: "middle",
    autoFit: "shrinkText",
    insets: { top: 1, right: 8, bottom: 1, left: 8 },
  };
  return p;
}

function line(slide, x, y, w, h, color, width = 2, dashed = false) {
  return slide.shapes.add({
    geometry: "line",
    position: { left: x, top: y, width: w, height: h },
    fill: "none",
    line: { style: dashed ? "dashed" : "solid", fill: color, width },
  });
}

function sectionTitle(slide, index, title, kicker) {
  slide.background.fill = C.bg;
  pill(slide, String(index).padStart(2, "0"), { left: 60, top: 42, width: 54, height: 30 }, C.navy, C.paper, { fontSize: 15 });
  textBox(slide, kicker.toUpperCase(), { left: 132, top: 44, width: 360, height: 26 }, {
    fontSize: 14, bold: true, color: C.teal, verticalAlignment: "middle",
  });
  textBox(slide, title, { left: 60, top: 78, width: 1160, height: 60 }, {
    fontSize: 39, bold: true, color: C.navy, verticalAlignment: "middle",
  });
  line(slide, 60, 139, 1160, 0, C.light, 1);
}

function footer(slide, page, note = "") {
  line(slide, 60, 666, 1160, 0, C.light, 1);
  textBox(slide, "OpenURMA × gem5", { left: 60, top: 678, width: 240, height: 18 }, {
    fontSize: 12, bold: true, color: C.slate,
  });
  if (note) {
    textBox(slide, note, { left: 330, top: 676, width: 820, height: 20 }, {
      fontSize: 11, color: C.slate, alignment: "center", autoFit: "shrinkText",
    });
  }
  textBox(slide, String(page).padStart(2, "0"), { left: 1170, top: 676, width: 50, height: 20 }, {
    fontSize: 12, bold: true, color: C.slate, alignment: "right",
  });
}

function addBullets(slide, items, position, opts = {}) {
  const s = shape(slide, "textbox", position, "none", "none", 0);
  s.text = makeNativeBulletParagraphs(items, {
    marginLeftPoints: opts.marginLeftPoints || 18,
    hangingPoints: opts.hangingPoints || 9,
    spaceAfterPoints: opts.spaceAfterPoints || 8,
  });
  s.text.style = {
    typeface: opts.typeface || FONT,
    fontSize: opts.fontSize || 23,
    color: opts.color || C.ink,
    autoFit: opts.autoFit || "none",
    wrap: "square",
    insets: opts.insets || { top: 0, right: 0, bottom: 0, left: 0 },
  };
  return s;
}

function codeBlock(slide, code, position, label, accent = C.teal, fontSize = 15) {
  const box = roundRect(slide, position, C.code, C.code, 15);
  box.shadow = "shadow-sm";
  pill(slide, label, { left: position.left + 14, top: position.top + 12, width: Math.min(position.width - 28, 350), height: 25 }, accent, C.paper, {
    fontSize: 11, typeface: MONO,
  });
  textBox(slide, code, {
    left: position.left + 18,
    top: position.top + 51,
    width: position.width - 36,
    height: position.height - 65,
  }, {
    typeface: MONO, fontSize, color: C.codeText, autoFit: "shrinkText",
  });
}

function notes(slide, text) {
  slide.speakerNotes.textFrame.setText(text);
  slide.speakerNotes.setVisible(true);
}

function sourceLabel(slide, text, position, color = C.slate) {
  textBox(slide, text, position, {
    typeface: MONO, fontSize: 11, color, autoFit: "shrinkText",
  });
}

// Slide 1 — cover
{
  const s = deck.slides.add();
  s.background.fill = C.navy;
  shape(s, "rect", { left: 0, top: 0, width: 14, height: 720 }, C.teal, C.teal, 0);
  shape(s, "ellipse", { left: 930, top: 0, width: 350, height: 350 }, "#102943", "#102943", 0);
  shape(s, "ellipse", { left: 1040, top: 40, width: 240, height: 240 }, "#12354A", "#12354A", 0);
  pill(s, "FULL-SYSTEM SIMULATION · 2026", { left: 70, top: 74, width: 295, height: 34 }, "#17324A", "#8FE1D9", {
    fontSize: 14, typeface: MONO, lineFill: "#27516A",
  });
  textBox(s, "OpenURMA × gem5\n双节点全系统仿真", { left: 70, top: 148, width: 795, height: 186 }, {
    fontSize: 62, bold: true, color: C.paper, verticalAlignment: "middle",
  });
  textBox(s, "官方 UDMA 软件栈复用、仿真硬件对接与 send_lat 验证", { left: 74, top: 355, width: 805, height: 54 }, {
    fontSize: 27, color: "#C7D2E2", verticalAlignment: "middle",
  });

  // Native cover illustration.
  const host0 = roundRect(s, { left: 890, top: 240, width: 145, height: 235 }, "#14283F", "#33506C", 18);
  const host1 = roundRect(s, { left: 1070, top: 240, width: 145, height: 235 }, "#14283F", "#33506C", 18);
  [host0, host1].forEach((h, idx) => {
    textBox(s, "NODE " + idx, { left: h.position.left + 18, top: 260, width: 109, height: 28 }, {
      typeface: MONO, fontSize: 14, bold: true, color: "#8FE1D9", alignment: "center",
    });
    const ys = [310, 349, 388, 427];
    const labels = ["Linux", "UMDK", "ubcore", "gem5 NIC"];
    ys.forEach((y, j) => {
      const layer = roundRect(s, { left: h.position.left + 18, top: y, width: 109, height: 28 }, j === 3 ? "#174A4A" : "#1E354D", "#33506C", 7);
      layer.text = labels[j];
      layer.text.style = {
        typeface: j === 3 ? MONO : FONT,
        fontSize: 12, bold: j === 3, color: C.paper,
        alignment: "center", verticalAlignment: "middle", autoFit: "shrinkText",
        insets: { top: 1, right: 3, bottom: 1, left: 3 },
      };
    });
  });
  line(s, 1035, 350, 35, 0, C.teal, 5);
  pill(s, "400 Gb/s", { left: 1012, top: 365, width: 80, height: 26 }, C.teal, C.paper, { fontSize: 11, typeface: MONO });
  line(s, 70, 522, 1140, 0, "#35506A", 1);
  const tags = [
    ["2 × ARM64 Linux", 70, 220],
    ["OLK 6.6", 307, 145],
    ["AtomicSimpleCPU · 3 GHz", 470, 300],
    ["400 Gbit/s 直连", 788, 200],
  ];
  tags.forEach(([label, x, w]) => pill(s, label, { left: x, top: 552, width: w, height: 38 }, "#14283F", "#D7E2EF", {
    fontSize: 15, lineFill: "#33506C",
  }));
  textBox(s, "工程说明 / 技术复盘", { left: 72, top: 642, width: 300, height: 25 }, {
    fontSize: 14, color: "#8394A9",
  });
  textBox(s, "OpenURMA: 0ae5dce · UMDK: 4eab3e4", { left: 765, top: 642, width: 450, height: 25 }, {
    typeface: MONO, fontSize: 12, color: "#8394A9", alignment: "right",
  });
  notes(s,
    "开场：这项工作的重点不是让命令看起来能跑，而是在两个独立的 gem5 全系统里，把官方 UDMA 用户态热路径一直闭合到仿真设备、跨节点链路和对端 CQE。\n\n" +
    "当前演示配置来源：\n" +
    path.join(LAB, "run-dual/run-manifest.txt") + "（profile、CPU、内存、400 Gbit/s、100 ns、同步量子）\n" +
    path.join(LAB, "README.md") + "（双节点运行方式与组件说明）"
  );
}

// Slide 2 — goals and acceptance
{
  const s = deck.slides.add();
  sectionTitle(s, 2, "目标不是“跑起命令”，而是闭合真实软件路径", "Goal & acceptance");

  textBox(s, "在没有物理 UDMA 设备的前提下，让官方软件栈在两个独立 Linux Guest 中完成一次可观测、可重复的双端通信。", {
    left: 60, top: 161, width: 1110, height: 62,
  }, { fontSize: 25, color: C.slate });

  const steps = [
    { n: "01", title: "两台主机真正独立", body: "两个 gem5 进程、两套 ARM64 内核与内存，各自保留串口 Shell。", color: C.blue },
    { n: "02", title: "官方软件看到真实设备", body: "ubcore/uburma 加载，urma_admin 枚举 openurma0，并显示 ACTIVE。", color: C.teal },
    { n: "03", title: "双端 send_lat 闭环", body: "服务端与客户端分别建资源、POST、传输、生成 CQE 并输出统计。", color: C.coral },
  ];
  steps.forEach((it, i) => {
    const x = 72 + i * 397;
    shape(s, "ellipse", { left: x, top: 263, width: 66, height: 66 }, it.color, it.color, 0);
    textBox(s, it.n, { left: x, top: 263, width: 66, height: 66 }, {
      typeface: MONO, fontSize: 20, bold: true, color: C.paper, alignment: "center", verticalAlignment: "middle",
    });
    if (i < 2) line(s, x + 78, 296, 300, 0, C.light, 3);
    textBox(s, it.title, { left: x, top: 352, width: 315, height: 42 }, {
      fontSize: 25, bold: true, color: C.navy,
    });
    textBox(s, it.body, { left: x, top: 408, width: 315, height: 112 }, {
      fontSize: 20, color: C.slate,
    });
  });

  roundRect(s, { left: 60, top: 557, width: 1160, height: 79 }, C.navy, C.navy, 14);
  pill(s, "本次演示配置", { left: 80, top: 577, width: 145, height: 36 }, C.teal, C.paper, { fontSize: 15 });
  textBox(s, "AtomicSimpleCPU · 1 core @ 3 GHz · 1 GB DDR3 · 400 Gbit/s direct UB · 100 ns propagation", {
    left: 247, top: 576, width: 920, height: 40,
  }, { typeface: MONO, fontSize: 18, color: "#D7E2EF", verticalAlignment: "middle", autoFit: "shrinkText" });
  footer(s, 2, "验收标准强调端到端路径，而不是单侧命令返回成功");
  notes(s,
    "讲解重点：验收要同时满足三件事——两个完整 Guest、官方软件枚举出 ACTIVE 设备、双端基准闭合。只满足其中一个都不足以证明 UDMA 热路径跑通。\n\n" +
    "来源：\n" +
    path.join(LAB, "README.md") + "（Two independent interactive hosts）\n" +
    path.join(LAB, "run-dual/run-manifest.txt") + "（cpu_mode=atomic, cpu_freq=3GHz, memory_size=1GB, peer_link_rate_gbps=400, peer_latency_ns=100）"
  );
}

// Slide 3 — architecture
{
  const s = deck.slides.add();
  sectionTitle(s, 3, "双 gem5 全系统：数据面与控制面分离", "System architecture");

  const hostX = [80, 760];
  const hostColors = [C.blue, C.teal];
  hostX.forEach((x, idx) => {
    roundRect(s, { left: x, top: 164, width: 440, height: 405 }, C.paper, C.light, 18, "node" + idx);
    pill(s, "NODE " + idx + " · 独立 gem5 进程", { left: x + 20, top: 183, width: 220, height: 32 }, hostColors[idx], C.paper, {
      typeface: MONO, fontSize: 13,
    });
    textBox(s, idx === 0 ? "10.0.0.1 / fe80::1" : "10.0.0.2 / fe80::2", {
      left: x + 250, top: 187, width: 165, height: 24,
    }, { typeface: MONO, fontSize: 12, color: C.slate, alignment: "right" });

    const layers = [
      ["ARM64 Linux + OLK 6.6", "#EDF3FA", C.blue],
      ["urma_perftest · urma_admin", C.tealPale, C.teal],
      ["liburma-udma.so", C.tealPale, C.teal],
      ["uburma.ko + ubcore.ko", C.tealPale, C.teal],
      ["openurma_ubcore.ko", C.coralPale, C.coral],
      ["NICTopologySC / UdmaSimAbi", "#18253A", C.paper],
    ];
    layers.forEach((layer, j) => {
      const y = 232 + j * 51;
      const rr = roundRect(s, { left: x + 24, top: y, width: 392, height: 39 }, layer[1], layer[1], 9);
      rr.text = layer[0];
      rr.text.style = {
        typeface: j === 5 ? MONO : FONT,
        fontSize: j === 5 ? 15 : 18,
        bold: j >= 3,
        color: layer[2],
        alignment: "center",
        verticalAlignment: "middle",
        autoFit: "shrinkText",
        insets: { top: 1, right: 5, bottom: 1, left: 5 },
      };
    });
  });

  line(s, 520, 480, 240, 0, C.teal, 8);
  shape(s, "rightArrow", { left: 620, top: 465, width: 44, height: 30 }, C.teal, C.teal, 0);
  pill(s, "UB payload", { left: 566, top: 433, width: 145, height: 30 }, C.teal, C.paper, { typeface: MONO, fontSize: 13 });
  textBox(s, "mmap peer ring\n400 Gb/s · 100 ns · arrival_tick", { left: 548, top: 502, width: 184, height: 55 }, {
    typeface: MONO, fontSize: 13, color: C.tealDark, alignment: "center",
  });

  line(s, 520, 285, 240, 0, C.amber, 3, true);
  shape(s, "rightArrow", { left: 620, top: 274, width: 38, height: 22 }, C.amber, C.amber, 0);
  pill(s, "OOB TCP", { left: 586, top: 239, width: 106, height: 28 }, C.amberPale, "#8A5A00", { typeface: MONO, fontSize: 12, lineFill: "#EAC878" });
  textBox(s, "握手 / 资源交换\n不计入测量区间", { left: 566, top: 306, width: 142, height: 48 }, {
    fontSize: 15, color: "#8A5A00", alignment: "center",
  });

  roundRect(s, { left: 154, top: 590, width: 972, height: 47 }, "#E9EEF4", "#D1DAE5", 12);
  textBox(s, "dist-gem5 同步：两端按 100 ns quantum 前进；链路到达时刻写入 peer slot，接收端只在虚拟时间到达后消费。", {
    left: 178, top: 600, width: 924, height: 28,
  }, { fontSize: 17, color: C.ink, alignment: "center", verticalAlignment: "middle", autoFit: "shrinkText" });
  footer(s, 3, "每个节点都能独立进入串口、运行命令与查看内核日志");
  notes(s,
    "架构图要先强调独立性：node0 与 node1 不是同一进程里的两个 endpoint，而是两个 gem5 全系统进程。UB payload 走带虚拟时间戳的共享 mmap ring；e1000/TCP 只负责 setup 和资源交换。\n\n" +
    "来源：\n" +
    path.join(LAB, "README.md") + "（Two independent interactive hosts）\n" +
    path.join(LAB, "run-dual/run-manifest.txt") + "（dma_transport、peer_link_rate_gbps、peer_latency_ns、sync_quantum_ns、oob_link_speed）\n" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/NICTopologySC.cc") + "（peer ring 与 UDMA 设备行为）"
  );
}

// Slide 4 — native editable component table
{
  const s = deck.slides.add();
  sectionTitle(s, 4, "哪些是官方代码，哪些是仿真适配", "Software provenance");

  const values = [
    ["归属", "组件 / 工具", "本次状态", "在端到端路径中的作用"],
    ["官方原样", "ubcore.ko + uburma.ko", "已加载", "UB 核心与用户态 ioctl / mmap 通道"],
    ["官方原样", "liburma-udma.so", "正在使用", "创建资源、编码 WQE、写 Doorbell、轮询 CQE"],
    ["官方原样", "urma_admin", "正在使用", "枚举 EID、设备与链路状态"],
    ["官方基础 + 适配", "urma_perftest", "正在使用", "保留 benchmark 主体；增加 gem5 时钟、双端同步与统计输出"],
    ["新增", "openurma_ubcore.ko", "已加载", "实现 ubcore device ops，固定页并通过 MMIO 下发资源描述"],
    ["新增", "NICTopologySC + UdmaSimAbi", "正在使用", "消费 Doorbell，DMA 读写 SQ / RQ / CQ，跨节点投递"],
    ["新增", "libummu.so.1 shim", "启动依赖", "满足本次仿真的映射 / 记账；不是完整 UMMU"],
    ["新增", "initramfs + run / sync / attach", "运行编排", "组装镜像、启动双节点、同步并接入串口"],
  ];
  const table = s.tables.add({
    rows: values.length,
    columns: 4,
    left: 60,
    top: 160,
    width: 1160,
    height: 447,
    columnWidths: [150, 275, 170, 565],
    values,
  });
  table.borders.assign({ style: "solid", fill: "#D7DEE7", width: 1 });
  table.styleOptions = { headerRow: true, bandedRows: false };
  for (let r = 0; r < values.length; r++) {
    table.rows[r].height = r === 0 ? 42 : 50.5;
    for (let c = 0; c < 4; c++) {
      const cell = table.getCell(r, c);
      cell.fill = r === 0 ? C.navy : (r % 2 ? C.paper : "#F8FAFC");
      cell.text.style = {
        typeface: c === 1 ? MONO : FONT,
        fontSize: r === 0 ? 17 : (c === 3 ? 16 : 17),
        bold: r === 0 || c === 0,
        color: r === 0 ? C.paper : C.ink,
        verticalAlignment: "middle",
        alignment: c === 0 || c === 2 ? "center" : "left",
        autoFit: "shrinkText",
      };
    }
    if (r > 0) {
      const kind = values[r][0];
      const color = kind === "官方原样" ? C.teal : (kind.includes("适配") ? C.amber : C.coral);
      table.getCell(r, 0).fill = color;
      table.getCell(r, 0).text.style = {
        typeface: FONT, fontSize: 16, bold: true, color: C.paper,
        alignment: "center", verticalAlignment: "middle", autoFit: "shrinkText",
      };
    }
  }
  roundRect(s, { left: 60, top: 620, width: 1160, height: 34 }, C.amberPale, "#E8C36F", 9);
  textBox(s, "关键边界：本次没有加载官方 kernel UDMA 硬件驱动；CONFIG_UB_UDMA 明确关闭，硬件行为由新增仿真设备承接。", {
    left: 78, top: 627, width: 1124, height: 20,
  }, { fontSize: 15, bold: true, color: "#785000", alignment: "center", autoFit: "shrinkText" });
  footer(s, 4);
  notes(s,
    "这是整份 PPT 最重要的边界页。前三项可以直接称为官方原样组件；urma_perftest 不能称为完全未修改，因为 common clock 和 perftest 的少量文件加入了 gem5 时钟、分布式同步、ROI 与统计逻辑。openurma_ubcore、NICTopologySC 的 UDMA 扩展、UMMU shim 和运行编排均为本工程新增。\n\n" +
    "来源：\n" +
    path.join(LAB, "build_olk66.sh") + ":70-85（CONFIG_UB_UDMA disabled；CONFIG_UB、CONFIG_UB_URMA enabled）\n" +
    path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/hw/udma") + "（官方 UDMA provider，工作树 clean）\n" +
    path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/tools/urma_perftest") + "（本地适配文件）\n" +
    path.join(OPENURMA, "integration/umdk/kmod/openurma_ubcore.c") + "（新增兼容驱动）\n" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/UdmaSimAbi.hh") + " 与 NICTopologySC.cc（新增仿真 ABI/设备行为）"
  );
}

// Slide 5 — control plane
{
  const s = deck.slides.add();
  sectionTitle(s, 5, "控制面：官方资源创建，仿真层接管硬件登记", "Control path");

  line(s, 103, 200, 0, 323, C.light, 5);
  const stages = [
    ["1", "官方 provider", "通过 ioctl / mmap 创建 Context、MR、JFC、JFR、Jetty。", C.teal],
    ["2", "官方 uburma / ubcore", "把用户请求分发到 openurma0 的 device ops。", C.teal],
    ["3", "新增 openurma_ubcore", "固定 Guest 页，映射 aperture，并发出 64 B 版本化 control record。", C.coral],
    ["4", "新增 NICTopologySC", "按 context / resource id 建动态资源表，记录 PGD、VA、深度与队列地址。", C.coral],
  ];
  stages.forEach((st, i) => {
    const y = 180 + i * 103;
    shape(s, "ellipse", { left: 75, top: y, width: 57, height: 57 }, st[3], st[3], 0);
    textBox(s, st[0], { left: 75, top: y, width: 57, height: 57 }, {
      typeface: MONO, fontSize: 19, bold: true, color: C.paper,
      alignment: "center", verticalAlignment: "middle",
    });
    textBox(s, st[1], { left: 157, top: y - 2, width: 268, height: 31 }, {
      fontSize: 22, bold: true, color: C.navy,
    });
    textBox(s, st[2], { left: 157, top: y + 32, width: 330, height: 58 }, {
      fontSize: 17, color: C.slate, autoFit: "shrinkText",
    });
  });

  codeBlock(s,
    [
      "ret = ou_pin_set_add(&seg->pins,",
      "    cfg->va, cfg->len, write);",
      "",
      "ou_emit_ctrl(od, OPENURMA_CTRL_ADD,",
      "    OPENURMA_CTRL_MR, ...",
      "    cfg->va, cfg->len, cfg->iova, ...);",
    ].join("\n"),
    { left: 540, top: 174, width: 680, height: 190 },
    "openurma_ubcore.c · MR register", C.coral, 16
  );
  codeBlock(s,
    [
      "msg.sequence = cpu_to_le32(++ctrl_seq);",
      "for (i = 0; i < 8; i++)",
      "    writeq(record[i],",
      "      aperture + CTRL_OFFSET + i * 8);",
      "wmb();",
    ].join("\n"),
    { left: 540, top: 382, width: 680, height: 173 },
    "openurma_ubcore.c · control record", C.coral, 16
  );
  sourceLabel(s, "src: openurma_ubcore.c:706–722, 355–392", { left: 550, top: 560, width: 600, height: 18 });

  roundRect(s, { left: 540, top: 590, width: 680, height: 53 }, C.bluePale, "#BCD2EC", 12);
  textBox(s, "资源地址与队列 ID 来自官方创建；仿真器由控制记录动态发现，不硬编码 Guest VA。", {
    left: 561, top: 602, width: 638, height: 30,
  }, { fontSize: 16, bold: true, color: "#245B93", alignment: "center", autoFit: "shrinkText" });
  footer(s, 5);
  notes(s,
    "控制面不是绕过官方资源模型。官方 UDMA provider 仍然发 ioctl/mmap；官方 uburma/ubcore 仍然负责用户态入口与核心对象。新增内核模块实现 openurma0 的 device ops，在注册 MR 时 pin 页，再把资源元数据写到仿真 aperture。NIC 用这些记录建立动态表，因此不需要硬编码用户虚拟地址或队列号。\n\n" +
    "源码证据：\n" +
    path.join(OPENURMA, "integration/umdk/kmod/openurma_ubcore.c") + ":706-722（pin MR 并 emit control）\n" +
    path.join(OPENURMA, "integration/umdk/kmod/openurma_ubcore.c") + ":355-392（64 B control record 原子提交）\n" +
    path.join(OPENURMA, "integration/umdk/kmod/openurma_udma_ctrl.h") + "（共享控制 ABI）"
  );
}

// Slide 6 — data plane
{
  const s = deck.slides.add();
  sectionTitle(s, 6, "数据面：官方 SQ / Doorbell 热路径真正执行", "Data path");

  const flow = [
    ["urma_perftest", "POST SEND_IMM", C.amber],
    ["liburma-udma", "编码 WQE + PI", C.teal],
    ["Doorbell MMIO", "写 sq->pi", C.teal],
    ["gem5 NIC", "DMA 读 SQ / payload", C.coral],
    ["peer + 对端", "DMA 写 RQ / CQE", C.coral],
  ];
  flow.forEach((f, i) => {
    const x = 60 + i * 235;
    const box = roundRect(s, { left: x, top: 165, width: 185, height: 76 }, C.paper, f[2], 13);
    textBox(s, f[0], { left: x + 12, top: 177, width: 161, height: 26 }, {
      typeface: i > 0 ? MONO : FONT, fontSize: 17, bold: true, color: C.navy, alignment: "center",
    });
    textBox(s, f[1], { left: x + 12, top: 207, width: 161, height: 22 }, {
      fontSize: 14, color: C.slate, alignment: "center", autoFit: "shrinkText",
    });
    if (i < flow.length - 1) shape(s, "rightArrow", { left: x + 191, top: 191, width: 36, height: 22 }, f[2], f[2], 0);
  });

  codeBlock(s,
    [
      "udma_u_post_one_wr(..., &wqe_addr, ...);",
      "UDMA_TO_DEVICE_BARRIER();",
      "udma_update_sq_db(sq);",
      "",
      "uint32_t *db = sq->db.addr + DB_OFFSET;",
      "*db = sq->pi;",
    ].join("\n"),
    { left: 60, top: 279, width: 555, height: 263 },
    "官方 UDMA provider · udma_u_jfs", C.teal, 15
  );
  codeBlock(s,
    [
      "if (off == doorbell) {",
      "  producer = loadLe32(data);",
      "  udma_enqueue_sq(jetty.id, producer);",
      "}",
      "udma_dma_ring(ctx, sq_va, ..., raw, ...);",
      "peer_send(..., payload, curTick());",
      "udma_dma_ring(ctx, cq_va, ..., cqe, true);",
    ].join("\n"),
    { left: 665, top: 279, width: 555, height: 263 },
    "新增 gem5 UDMA engine · NICTopologySC", C.coral, 15
  );
  sourceLabel(s, "udma_u_jfs.c:821–837; udma_u_jfs.h:129–133", { left: 76, top: 548, width: 520, height: 18 });
  sourceLabel(s, "NICTopologySC.cc:2402–2421, 1440–1523, 1278–1287", { left: 680, top: 548, width: 520, height: 18 });

  roundRect(s, { left: 60, top: 590, width: 1160, height: 52 }, C.tealPale, "#A8D9D4", 12);
  textBox(s, "结论：不是 mock 直接返回完成；WQE、payload 与 CQE 都落在 Guest memory，仿真设备通过 gem5 memory request 读写。", {
    left: 82, top: 602, width: 1116, height: 29,
  }, { fontSize: 18, bold: true, color: C.tealDark, alignment: "center", autoFit: "shrinkText" });
  footer(s, 6);
  notes(s,
    "数据面关键证据有两段。第一段是官方 UDMA provider：它逐个生成 WQE，执行 device barrier，然后把 SQ producer 写入 doorbell。第二段是新增的 NICTopologySC UDMA engine：捕获 doorbell，按官方布局从 Guest SQ 做 DMA read，再读 payload、跨 peer ring 投递，并在对端 Guest CQ 写 CQE。官方 provider 的热路径因此是真实执行的。\n\n" +
    "源码证据：\n" +
    path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/hw/udma/udma_u_jfs.c") + ":821-837\n" +
    path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/hw/udma/udma_u_jfs.h") + ":129-133\n" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/NICTopologySC.cc") + ":2402-2421（doorbell）\n" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/NICTopologySC.cc") + ":1440-1523（SQ/payload DMA 与 peer send）\n" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/NICTopologySC.cc") + ":1278-1287（CQE 写回）"
  );
}

// Slide 7 — link timing
{
  const s = deck.slides.add();
  sectionTitle(s, 7, "链路与虚拟时间：按 arrival_tick 建立因果关系", "Link timing");

  roundRect(s, { left: 60, top: 165, width: 720, height: 112 }, C.navy, C.navy, 17);
  textBox(s, "arrival = max(tx, stage_free) + ceil(bits / 400 Gb/s) + 0 ns + 100 ns", {
    left: 86, top: 190, width: 670, height: 45,
  }, { typeface: MONO, fontSize: 19, bold: true, color: C.paper, alignment: "center", verticalAlignment: "middle", autoFit: "shrinkText" });
  textBox(s, "serialization", { left: 368, top: 238, width: 115, height: 20 }, {
    typeface: MONO, fontSize: 11, color: "#8FE1D9", alignment: "center",
  });
  textBox(s, "switch", { left: 559, top: 238, width: 68, height: 20 }, {
    typeface: MONO, fontSize: 11, color: "#FFD996", alignment: "center",
  });
  textBox(s, "propagation", { left: 665, top: 238, width: 93, height: 20 }, {
    typeface: MONO, fontSize: 11, color: "#F8A3A3", alignment: "center",
  });

  pill(s, "当前参数", { left: 820, top: 166, width: 120, height: 31 }, C.amber, C.paper, { fontSize: 14 });
  addBullets(s, [
    "400 Gbit/s，1 个 serialization stage",
    "传播 100 ns；switch delay = 0",
    "固定 SQ / DMA service delay 全为 0",
    "同步 quantum = 100 ns",
  ], { left: 820, top: 211, width: 380, height: 155 }, { fontSize: 19, spaceAfterPoints: 5 });

  // Timeline
  textBox(s, "NODE 0", { left: 60, top: 332, width: 90, height: 25 }, {
    typeface: MONO, fontSize: 14, bold: true, color: C.blue,
  });
  textBox(s, "NODE 1", { left: 60, top: 465, width: 90, height: 25 }, {
    typeface: MONO, fontSize: 14, bold: true, color: C.teal,
  });
  line(s, 150, 345, 1000, 0, C.blue, 4);
  line(s, 150, 478, 1000, 0, C.teal, 4);
  const xs = [220, 400, 660, 930];
  const topLabels = ["doorbell", "DMA read", "peer_send(tx_tick)", "advance"];
  const bottomLabels = ["waiting", "sync quantum", "curTick ≥ arrival", "DMA + CQE"];
  xs.forEach((x, i) => {
    shape(s, "ellipse", { left: x, top: 335, width: 20, height: 20 }, C.blue, C.blue, 0);
    shape(s, "ellipse", { left: x, top: 468, width: 20, height: 20 }, i < 2 ? C.light : C.teal, i < 2 ? C.light : C.teal, 0);
    textBox(s, topLabels[i], { left: x - 47, top: 362, width: 115, height: 38 }, {
      typeface: MONO, fontSize: 12, color: C.slate, alignment: "center", autoFit: "shrinkText",
    });
    textBox(s, bottomLabels[i], { left: x - 55, top: 498, width: 130, height: 38 }, {
      typeface: MONO, fontSize: 12, color: C.slate, alignment: "center", autoFit: "shrinkText",
    });
  });
  shape(s, "downArrow", { left: 690, top: 388, width: 40, height: 65 }, C.coral, C.coral, 0);
  pill(s, "arrival_tick", { left: 642, top: 405, width: 136, height: 29 }, C.coralPale, C.coral, { typeface: MONO, fontSize: 12, lineFill: "#F0A1A1" });

  roundRect(s, { left: 60, top: 568, width: 1160, height: 76 }, C.paper, C.light, 13);
  line(s, 86, 592, 220, 0, C.teal, 6);
  textBox(s, "UB payload：带到达时刻的 peer ring", { left: 326, top: 580, width: 360, height: 26 }, {
    fontSize: 17, bold: true, color: C.ink,
  });
  line(s, 710, 592, 135, 0, C.amber, 3, true);
  textBox(s, "OOB TCP：只做 setup", { left: 860, top: 580, width: 245, height: 26 }, {
    fontSize: 17, bold: true, color: C.ink,
  });
  textBox(s, "当前截图运行 pipe_data=0：payload 由增强 NICTopologySC UDMA engine 处理，38 模块 SystemC payload pipeline 未开启。", {
    left: 85, top: 613, width: 1095, height: 21,
  }, { fontSize: 13, color: C.slate, alignment: "center", autoFit: "shrinkText" });
  footer(s, 7);
  notes(s,
    "链路模型的核心是 arrival_tick，而不是宿主机墙钟。发送端根据链路空闲时刻、按 400 Gbit/s 计算的序列化时间、switch delay 和 100 ns 传播时间写入到达时刻。接收端只有在自身 curTick 到达该时刻后才消费；100 ns 同步量子与正 lookahead 配套。\n\n" +
    "本次 manifest 中 direct_wqe_latency、sq_fetch_latency、sq_wqebb_latency、payload_dma_latency 均为 0，因此没有用额外固定延迟去拟合实测。另需明确 pipe_data=0：完整 38-module SystemC 模型已集成，但本次 payload 没有走它。\n\n" +
    "来源：\n" +
    path.join(LAB, "run-dual/run-manifest.txt") + "（peer_link_rate_gbps、peer_serialization_stages、peer_switch_delay、peer_latency_ns、sync_quantum_ns、pipe_data、各 fixed latency）\n" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/NICTopologySC.cc") + ":2239-2250（peer timing）\n" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/NICTopologySC.cc") + ":2147-2187（causal drain）"
  );
}

// Slide 8 — boot and active evidence
{
  const s = deck.slides.add();
  sectionTitle(s, 8, "启动证据：官方模块加载，openurma0 进入 ACTIVE", "Boot evidence");

  pill(s, "Guest 启动日志", { left: 60, top: 160, width: 145, height: 30 }, C.navy, C.paper, { fontSize: 13 });
  roundRect(s, { left: 60, top: 196, width: 889, height: 257 }, C.code, C.code, 13);
  s.images.add({
    blob: bootBytes,
    contentType: "image/png",
    alt: "Guest boot log showing official ubcore and uburma modules plus openurma_ubcore registration",
    fit: "contain",
    position: { left: 72, top: 206, width: 865, height: 237 },
    geometry: "roundRect",
    borderRadius: 8,
  });

  textBox(s, "日志里确认三件事", { left: 981, top: 170, width: 230, height: 36 }, {
    fontSize: 22, bold: true, color: C.navy,
  });
  const evidence = [
    ["官方", "ubcore.ko + uburma.ko\n已加载", C.teal],
    ["新增", "openurma_ubcore.ko\n已加载", C.coral],
    ["设备", "openurma0 · UB\nABI=udma", C.blue],
  ];
  evidence.forEach((e, i) => {
    pill(s, e[0], { left: 981, top: 226 + i * 66, width: 60, height: 27 }, e[2], C.paper, { fontSize: 12 });
    textBox(s, e[1], { left: 1050, top: 223 + i * 66, width: 165, height: 42 }, {
      fontSize: 13, bold: true, color: C.ink, verticalAlignment: "middle", autoFit: "shrinkText", wrap: "none",
    });
  });

  pill(s, "urma_admin show", { left: 60, top: 474, width: 160, height: 30 }, C.teal, C.paper, { typeface: MONO, fontSize: 13 });
  roundRect(s, { left: 60, top: 512, width: 1160, height: 126 }, C.code, C.code, 13);
  s.images.add({
    blob: activeBytes,
    contentType: "image/png",
    alt: "urma_admin output showing openurma0 with ACTIVE link",
    fit: "contain",
    position: { left: 72, top: 520, width: 1136, height: 111 },
    geometry: "roundRect",
    borderRadius: 7,
  });
  pill(s, "ACTIVE", { left: 1090, top: 477, width: 104, height: 30 }, C.teal, C.paper, { typeface: MONO, fontSize: 14 });
  footer(s, 8, "截图来自本次双节点全系统运行");
  notes(s,
    "先看启动日志：ipv6、官方 ubcore、官方 uburma 先加载；之后 openurma_ubcore 注册 openurma0，并明确打印 ABI=udma。再看 urma_admin：用户态工具从官方 API 枚举到了 eid0 和 ACTIVE link。这里的 ACTIVE 表示仿真设备的端到端就绪条件已经满足。\n\n" +
    "用户截图：\n" +
    IMG_BOOT + "\n" +
    IMG_ACTIVE + "\n" +
    "相关源码：\n" +
    path.join(OPENURMA, "integration/umdk/kmod/openurma_ubcore.c") + ":1366-1423（以 driver_name=udma 注册 openurma0）"
  );
}

// Slide 9 — result
{
  const s = deck.slides.add();
  sectionTitle(s, 9, "128 B send_lat：双端资源与数据路径闭环", "Demonstration result");

  textBox(s, "node0  ou-lat-server --profile ctp-rm-send-imm-i128 20 128 21115", {
    left: 66, top: 155, width: 560, height: 24,
  }, { typeface: MONO, fontSize: 13, color: C.blue, autoFit: "shrinkText" });
  textBox(s, "node1  ou-lat-client --profile ctp-rm-send-imm-i128 20 128 21115", {
    left: 654, top: 155, width: 560, height: 24,
  }, { typeface: MONO, fontSize: 13, color: C.teal, alignment: "right", autoFit: "shrinkText" });

  roundRect(s, { left: 60, top: 190, width: 1160, height: 223 }, C.code, C.code, 13);
  s.images.add({
    blob: resultBytes,
    contentType: "image/png",
    alt: "128 byte send_lat output with 20 samples and latency statistics",
    fit: "contain",
    position: { left: 70, top: 200, width: 1140, height: 203 },
    geometry: "roundRect",
    borderRadius: 8,
  });

  const metrics = [
    ["44.03", "min / median", C.blue],
    ["52.15", "average", C.teal],
    ["35.16", "stdev", C.amber],
    ["205.42", "max / p99", C.coral],
  ];
  metrics.forEach((m, i) => {
    const x = 72 + i * 285;
    if (i > 0) line(s, x - 21, 447, 0, 92, C.light, 1);
    textBox(s, m[0], { left: x, top: 439, width: 240, height: 50 }, {
      typeface: MONO, fontSize: 34, bold: true, color: m[2], alignment: "center", verticalAlignment: "middle",
    });
    textBox(s, m[1] + " [µs]", { left: x, top: 496, width: 240, height: 28 }, {
      typeface: MONO, fontSize: 14, color: C.slate, alignment: "center",
    });
  });

  roundRect(s, { left: 60, top: 560, width: 1160, height: 80 }, C.amberPale, "#E8C36F", 13);
  pill(s, "如何解读", { left: 82, top: 579, width: 112, height: 34 }, C.amber, C.paper, { fontSize: 14 });
  richTextBox(s, [[
    { run: "20 样本 → p99 秩 = ceil(0.99 × 20) = 20 → ", textStyle: { color: C.ink } },
    { run: "p99 必然等于最大值", textStyle: { bold: true, color: C.coral } },
  ]], { left: 220, top: 572, width: 966, height: 27 }, {
    fontSize: 17, verticalAlignment: "middle", autoFit: "shrinkText",
  });
  textBox(s, "这页用于证明端到端路径闭合；尾延迟需要 ≥1,001 / 16,384 样本再评估。", {
    left: 220, top: 603, width: 966, height: 23,
  }, { fontSize: 15, color: C.slate, verticalAlignment: "middle", autoFit: "shrinkText" });
  footer(s, 9, "5 个同步 warm-up delta 已排除；measured samples = 20");
  notes(s,
    "这次 128 B 演示中，node0 运行 server，node1 运行 client；wrapper 负责双端同步并调用同一份适配后的官方 urma_perftest。结果显示 5 个同步 warm-up delta 被排除，20 个样本进入统计。min/median=44.03 us，avg=52.15 us，stdev=35.16 us，max/p99=205.42 us。\n\n" +
    "解释 p99：perftest 的 nearest-rank 算法下，20 个样本的 p99 取第 20 个，也就是最大样本，因此不能拿这一页评价 tail stability。要看 p99/p99.9，需要至少上千到 16384 样本。\n\n" +
    "用户截图：\n" + IMG_RESULT + "\n" +
    "运行封装：\n" + path.join(LAB, "run-latency.sh") + ":101-159\n" +
    "统计适配：\n" + path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/tools/urma_perftest/perftest_run_test.c")
  );
}

// Slide 10 — status and next
{
  const s = deck.slides.add();
  sectionTitle(s, 10, "结论：官方热路径已跑通，剩余工作边界清晰", "Conclusion & next");

  const cols = [
    {
      x: 60, color: C.teal, title: "已经做到",
      items: [
        "两个独立 ARM64 全系统 Guest，可分别进入 Shell",
        "官方 ubcore / uburma 与官方 UDMA provider 在路径中",
        "官方 SQ、Doorbell、CQ poll 热路径真实执行",
        "Guest memory DMA、跨节点投递与对端 CQE 闭环",
        "400 Gbit/s arrival timing + 100 ns 分布式同步",
      ],
    },
    {
      x: 455, color: C.amber, title: "当前边界",
      items: [
        "官方 kernel UDMA 硬件驱动尚未加载",
        "urma_perftest 为官方主体 + 仿真时钟/同步适配",
        "当前覆盖 normal pinned、duplex Jetty、CTP/RM SEND_IMM",
        "UMMU shim 只实现本次所需能力",
        "本次 payload：pipe_data=0",
      ],
    },
    {
      x: 850, color: C.coral, title: "下一阶段",
      items: [
        "n=16,384 复测 tail，并做 2–4096 B 扫描",
        "恢复 O3 / cache / DDR 正式服务器 ROI 配置",
        "加入显式 L1 switch，再比较结构性趋势",
        "补齐 non-pin / hugepage / simplex 与严格 drain",
        "收缩兼容层，最终做到官方驱动不改",
      ],
    },
  ];
  cols.forEach((col) => {
    line(s, col.x, 171, 320, 0, col.color, 6);
    textBox(s, col.title, { left: col.x, top: 188, width: 320, height: 42 }, {
      fontSize: 27, bold: true, color: C.navy,
    });
    addBullets(s, col.items, { left: col.x, top: 245, width: 330, height: 300 }, {
      fontSize: 17, spaceAfterPoints: 9, marginLeftPoints: 16, hangingPoints: 8,
    });
  });

  line(s, 60, 558, 1160, 0, C.light, 1);
  textBox(s, "终极目标", { left: 60, top: 582, width: 150, height: 32 }, {
    fontSize: 20, bold: true, color: C.coral,
  });
  textBox(s, "官方驱动与用户态代码尽量不改", { left: 230, top: 575, width: 320, height: 45 }, {
    fontSize: 22, bold: true, color: C.navy, alignment: "center", verticalAlignment: "middle",
  });
  shape(s, "rightArrow", { left: 570, top: 585, width: 96, height: 25 }, C.coral, C.coral, 0);
  textBox(s, "缺少的硬件行为全部由仿真器实现并统一对接", { left: 690, top: 575, width: 510, height: 45 }, {
    fontSize: 22, bold: true, color: C.navy, alignment: "center", verticalAlignment: "middle", autoFit: "shrinkText",
  });
  footer(s, 10, "当前版本已经把“官方用户态热路径 + 官方核心模块 + 仿真硬件”连成一条可运行链路");
  notes(s,
    "收尾时分三栏讲。左栏是已经有代码和运行证据的能力；中栏是必须诚实保留的边界；右栏是下一阶段从功能闭环走向正式趋势研究的顺序。最终目标仍是：尽量不改官方驱动和用户态代码，把缺失硬件行为集中放入仿真器，并通过统一 ABI 对接。\n\n" +
    "范围证据：\n" +
    path.join(LAB, "run-dual/run-manifest.txt") + "\n" +
    path.join(LAB, "README.md") + "\n" +
    path.join(LAB, "build_olk66.sh") + ":70-85\n" +
    path.join(OPENURMA, "integration/umdk/kmod/openurma_ubcore.c") + "\n" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/NICTopologySC.cc") + "\n" +
    path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/tools/urma_perftest")
  );
}

const requirements = {
  explicitTotalSlideCount: 10,
  requiredNativeTableOwnerSlides: [4],
  requiredNativeChartOwnerSlides: [],
};
const fontPolicy = {
  basis: "design",
  families: [FONT, MONO],
};
const stagingDir = path.join(BUILD_DIR, ".codex-finalizer-" + VERSION);
await fs.mkdir(stagingDir, { recursive: true });
const candidatePath = path.join(stagingDir, "candidate.pptx");
await (await PresentationFile.exportPptx(deck)).save(candidatePath);

const result = await finalizePresentation({
  ...requirements,
  workspaceDir: PRESENTATION_DIR,
  candidatePath,
  finalPath: FINAL_PPTX,
  pythonExecutable: RUNTIME_PYTHON,
  integrityValidatorPath: path.join(SKILL_DIR, "container_tools/inspect_presentation_package_integrity.py"),
  layoutValidatorPath: path.join(SKILL_DIR, "container_tools/inspect_presentation_layout_geometry.py"),
  layoutArgs: [
    "--expected-slide-size-emu", "12192000,6858000",
    "--validate-bullet-geometry",
    "--validate-heading-fit",
    "--require-native-table-slide", "4",
  ],
  requiredNativeTableOwnerSlides: [4],
  requiredNativeChartOwnerSlides: [],
  fontPolicy,
  verifyArtifactToolImport: true,
  receiptPath: path.join(stagingDir, path.basename(FINAL_PPTX) + ".validation.json"),
});

console.log(JSON.stringify({ finalPath: FINAL_PPTX, result }, null, 2));

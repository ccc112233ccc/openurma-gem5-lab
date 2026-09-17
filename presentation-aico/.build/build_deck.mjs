import fs from "node:fs/promises";
import path from "node:path";
import { pathToFileURL } from "node:url";
import { Presentation, PresentationFile } from "@oai/artifact-tool";
import sharp from "sharp";

const ROOT = "/Users/caobo/workspace";
const LAB = path.join(ROOT, "openurma-gem5-lab");
const OPENURMA = path.join(ROOT, "OpenURMA");
const AICO = path.join(ROOT, "AICO-PPT");
const PRESENTATION_DIR = path.join(LAB, "presentation-aico");
const BUILD_DIR = path.join(PRESENTATION_DIR, ".build");
const OUTPUT_DIR = path.join(PRESENTATION_DIR, "output");
const SKILL_DIR = "/Users/caobo/.codex/plugins/cache/openai-primary-runtime/presentations/26.905.11957/skills/presentations";
const RUNTIME_NODE = "/Users/caobo/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/bin/node";
const RUNTIME_NODE_MODULES = "/Users/caobo/.cache/codex-runtimes/codex-primary-runtime/dependencies/node/node_modules";
const RUNTIME_PYTHON = "/Users/caobo/.cache/codex-runtimes/codex-primary-runtime/dependencies/python/bin/python3";
const VERSION = process.env.DECK_VERSION || "v1";
const FINAL_PPTX = path.join(OUTPUT_DIR, `OpenURMA_gem5_AICO_style_${VERSION}.pptx`);

const IMG_COVER = path.join(AICO, "assets/huawei-refs/covers/背景-深灰纹理.jpeg");
const IMG_COVER_GEOM = path.join(AICO, "assets/huawei-refs/components/装饰-白色几何线框1.png");
const IMG_BOOT = "/var/folders/yp/xpx6mk9x5xzgmpf95djdjwwc0000gn/T/codex-clipboard-d4ec0f4e-e2a8-4875-a56f-49109133a2d3.png";
const IMG_ACTIVE = "/var/folders/yp/xpx6mk9x5xzgmpf95djdjwwc0000gn/T/codex-clipboard-f7af11e0-5108-4c5d-b56e-c98f871328f4.png";
const IMG_RESULT = "/var/folders/yp/xpx6mk9x5xzgmpf95djdjwwc0000gn/T/codex-clipboard-e0a2b538-17e5-456c-815e-2728573719f7.png";

// Noto Sans SC is the AICO HTML font. Hiragino Sans GB is used here because it
// is available to the local PowerPoint renderer and preserves Chinese weight.
const FONT = "Hiragino Sans GB";
const MONO = "PT Mono";
const C = {
  red: "#B5333B",
  red2: "#CF6B72",
  redPale: "#FDF0F1",
  redWash: "#FBF4EE",
  blue: "#1565C0",
  grayBlue: "#566472",
  ink: "#1A1A1C",
  body: "#585860",
  dark: "#15171C",
  code: "#20242C",
  codeText: "#EDEFF2",
  paper: "#FFFFFF",
  bg: "#FAFAFA",
  border: "#D5D9DE",
  grid: "#E7E7E7",
  muted: "#8A8A92",
  pale: "#F2F4F6",
  gold: "#E8A33D",
};

const { finalizePresentation } = await import(
  pathToFileURL(path.join(SKILL_DIR, "container_tools/artifact_tool_utils.mjs")).href
);

process.env.RUNTIME_NODE = RUNTIME_NODE;
process.env.RUNTIME_NODE_MODULES = RUNTIME_NODE_MODULES;

await fs.mkdir(BUILD_DIR, { recursive: true });
await fs.mkdir(OUTPUT_DIR, { recursive: true });

const [coverBytes, coverGeomBytes, bootSource, activeSource, resultBytes] = await Promise.all([
  fs.readFile(IMG_COVER),
  fs.readFile(IMG_COVER_GEOM),
  fs.readFile(IMG_BOOT),
  fs.readFile(IMG_ACTIVE),
  fs.readFile(IMG_RESULT),
]);
const [bootBytes, activeBytes] = await Promise.all([
  sharp(bootSource).extract({ left: 0, top: 430, width: 2174, height: 620 }).png().toBuffer(),
  sharp(activeSource).extract({ left: 0, top: 0, width: 1640, height: 200 }).png().toBuffer(),
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
    fontSize: opts.fontSize || 20,
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

function roundRect(slide, position, fill = C.paper, lineFill = C.border, radius = 14, lineWidth = 1, name) {
  const s = shape(slide, "roundRect", position, fill, lineFill, lineWidth, name);
  s.borderRadius = radius;
  return s;
}

function line(slide, x, y, w, h, color = C.border, width = 1, dashed = false) {
  return slide.shapes.add({
    geometry: "line",
    position: { left: x, top: y, width: w, height: h },
    fill: "none",
    line: { style: dashed ? "dashed" : "solid", fill: color, width },
  });
}

function connect(slide, from, to, opts = {}) {
  return slide.shapes.connect(from, to, {
    kind: opts.kind || "straight",
    fromSide: opts.fromSide || "right",
    toSide: opts.toSide || "left",
    line: { style: opts.dashed ? "dashed" : "solid", fill: opts.color || C.grayBlue, width: opts.width || 2 },
    ...(opts.arrow === false ? {} : opts.bidirectional
      ? {
          head: { type: "arrow", width: "med", length: "med" },
          tail: { type: "arrow", width: "med", length: "med" },
        }
      : { tail: { type: "arrow", width: "med", length: "med" } }),
  });
}

function addImage(slide, bytes, contentType, position, alt, fit = "contain", geometry = "rect", radius = 0) {
  return slide.images.add({
    blob: bytes,
    contentType,
    alt,
    fit,
    position,
    geometry,
    ...(radius ? { borderRadius: radius } : {}),
  });
}

function addNotes(slide, text) {
  slide.speakerNotes.textFrame.setText(text);
  slide.speakerNotes.setVisible(true);
}

function header(slide, section, title, page) {
  slide.background.fill = C.bg;
  textBox(slide, section, { left: 60, top: 36, width: 440, height: 20 }, {
    typeface: MONO, fontSize: 13, bold: true, color: C.red, wrap: "none",
  });
  textBox(slide, title, { left: 60, top: 72, width: 1160, height: 58 }, {
    fontSize: 36, bold: true, color: C.ink, verticalAlignment: "middle", autoFit: "shrinkText",
  });
  line(slide, 60, 140, 1160, 0, C.grid, 1);
  footer(slide, page);
}

function footer(slide, page) {
  textBox(slide, "OpenURMA × gem5", { left: 60, top: 687, width: 240, height: 16 }, {
    typeface: MONO, fontSize: 10, color: C.grayBlue, wrap: "none",
  });
  textBox(slide, String(page).padStart(2, "0"), { left: 1160, top: 687, width: 60, height: 16 }, {
    typeface: MONO, fontSize: 10, color: C.grayBlue, alignment: "right", wrap: "none",
  });
}

function labelText(slide, text, position, color = C.red) {
  return textBox(slide, text, position, { typeface: MONO, fontSize: 12, bold: true, color, wrap: "none" });
}

function boxWithText(slide, text, position, opts = {}) {
  const b = roundRect(slide, position, opts.fill || C.paper, opts.lineFill || C.border, opts.radius || 12, opts.lineWidth ?? 1, opts.name);
  b.text = text;
  b.text.style = {
    typeface: opts.typeface || FONT,
    fontSize: opts.fontSize || 18,
    bold: opts.bold ?? true,
    color: opts.color || C.ink,
    alignment: opts.alignment || "center",
    verticalAlignment: opts.verticalAlignment || "middle",
    autoFit: opts.autoFit || "shrinkText",
    wrap: opts.wrap || "square",
    insets: opts.insets || { top: 8, right: 10, bottom: 8, left: 10 },
  };
  return b;
}

function codeBlock(slide, title, code, position, opts = {}) {
  const box = roundRect(slide, position, C.code, C.code, 12, 0);
  shape(slide, "rect", { left: position.left, top: position.top, width: 5, height: position.height }, opts.accent || C.red, opts.accent || C.red, 0);
  textBox(slide, title, { left: position.left + 18, top: position.top + 14, width: position.width - 36, height: 22 }, {
    typeface: MONO, fontSize: 12, bold: true, color: opts.titleColor || C.red2, wrap: "none", autoFit: "shrinkText",
  });
  textBox(slide, code, { left: position.left + 18, top: position.top + 48, width: position.width - 36, height: position.height - 62 }, {
    typeface: MONO, fontSize: opts.fontSize || 14, color: C.codeText, autoFit: "shrinkText",
  });
  return box;
}

function metric(slide, value, label, position, opts = {}) {
  const group = roundRect(slide, position, opts.fill || C.paper, opts.lineFill || C.border, 12, 1);
  textBox(slide, value, { left: position.left + 14, top: position.top + 10, width: position.width - 28, height: 48 }, {
    typeface: MONO, fontSize: opts.valueSize || 34, bold: true, color: opts.valueColor || C.red,
    verticalAlignment: "middle", alignment: opts.alignment || "left", autoFit: "shrinkText",
  });
  textBox(slide, label, { left: position.left + 14, top: position.top + 58, width: position.width - 28, height: 28 }, {
    fontSize: 15, color: C.body, alignment: opts.alignment || "left", autoFit: "shrinkText",
  });
  return group;
}

function cornerBrackets(slide, position, color = C.red) {
  const l = 18;
  const w = 2;
  line(slide, position.left, position.top, l, 0, color, w);
  line(slide, position.left, position.top, 0, l, color, w);
  line(slide, position.left + position.width - l, position.top, l, 0, color, w);
  line(slide, position.left + position.width, position.top, 0, l, color, w);
  line(slide, position.left, position.top + position.height, l, 0, color, w);
  line(slide, position.left, position.top + position.height - l, 0, l, color, w);
  line(slide, position.left + position.width - l, position.top + position.height, l, 0, color, w);
  line(slide, position.left + position.width, position.top + position.height - l, 0, l, color, w);
}

// 1. Cover
{
  const s = deck.slides.add();
  addImage(s, coverBytes, "image/jpeg", { left: 0, top: 0, width: 1280, height: 720 }, "AICO-PPT dark technical texture", "cover");
  shape(s, "rect", { left: 0, top: 0, width: 1280, height: 720 }, "#11151C", "#11151C", 0).opacity = 0.18;
  addImage(s, coverGeomBytes, "image/png", { left: 650, top: 30, width: 610, height: 520 }, "white geometric network artwork", "contain");
  shape(s, "rect", { left: 72, top: 78, width: 38, height: 5 }, C.red, C.red, 0);
  textBox(s, "OPENURMA × GEM5 / SYSTEMS TECH TALK", { left: 126, top: 69, width: 520, height: 24 }, {
    typeface: MONO, fontSize: 14, bold: true, color: "#F0C3C6", wrap: "none",
  });
  textBox(s, "把 OpenURMA", { left: 72, top: 164, width: 650, height: 76 }, {
    fontSize: 56, bold: true, color: C.paper, verticalAlignment: "middle",
  });
  textBox(s, "跑进", { left: 72, top: 239, width: 170, height: 76 }, {
    fontSize: 56, bold: true, color: C.paper, verticalAlignment: "middle",
  });
  textBox(s, "gem5", { left: 225, top: 239, width: 255, height: 76 }, {
    typeface: MONO, fontSize: 56, bold: true, color: C.red2, verticalAlignment: "middle",
  });
  textBox(s, "双节点全系统仿真、UDMA 设备模型与 SEND_IMM 路径", { left: 76, top: 342, width: 650, height: 42 }, {
    fontSize: 22, color: "#D8DDE5", autoFit: "shrinkText",
  });

  const coverNode0 = boxWithText(s, "node 0\nserver", { left: 735, top: 508, width: 150, height: 74 }, {
    fill: "#171B22", lineFill: "#A6AFBB", color: C.paper, typeface: MONO, fontSize: 15, radius: 9,
  });
  const coverNode1 = boxWithText(s, "node 1\nclient", { left: 1060, top: 508, width: 150, height: 74 }, {
    fill: "#171B22", lineFill: C.red2, color: C.paper, typeface: MONO, fontSize: 15, radius: 9, lineWidth: 2,
  });
  connect(s, coverNode0, coverNode1, { color: C.red2, width: 3, bidirectional: true });
  boxWithText(s, "400G / 100 ns", { left: 902, top: 524, width: 140, height: 42 }, {
    fill: C.red, lineFill: C.red, color: C.paper, typeface: MONO, fontSize: 12, radius: 21,
  }).bringToFront();
  line(s, 76, 604, 570, 0, "#737E8D", 1);
  textBox(s, "技术分享 · 2026.09", { left: 76, top: 624, width: 260, height: 24 }, {
    typeface: MONO, fontSize: 14, color: C.paper,
  });
  textBox(s, "ARM64 FULL SYSTEM  /  OFFICIAL UDMA PROVIDER  /  DUAL NODE", { left: 650, top: 626, width: 560, height: 22 }, {
    typeface: MONO, fontSize: 11, color: "#AEB7C5", alignment: "right", autoFit: "shrinkText", wrap: "none",
  });
  addNotes(s,
    "这是一场系统技术分享。开场问题是：在没有真实 UDMA 硬件的情况下，如何让官方 provider 继续执行 WQE、doorbell 与 CQ 轮询，并让两套 gem5 guest 真正完成一次跨节点 SEND_IMM。\n\n" +
    "视觉参考：" + path.join(AICO, "references/design-system.md") + "、" + path.join(AICO, "references/huawei-style.md") + "。\n" +
    "封面素材：" + IMG_COVER + "、" + IMG_COVER_GEOM + "。本 deck 未沿用 AICO 模板自带的 Huawei Logo、密级与免责声明。"
  );
}

// 2. TOC
{
  const s = deck.slides.add();
  s.background.fill = C.paper;
  textBox(s, "目录 / CONTENTS", { left: 60, top: 44, width: 300, height: 20 }, {
    typeface: MONO, fontSize: 13, bold: true, color: C.red,
  });
  textBox(s, "今天讲四件事", { left: 60, top: 82, width: 400, height: 55 }, {
    fontSize: 39, bold: true, color: C.ink,
  });
  line(s, 60, 146, 76, 0, C.red, 3);

  const panel = roundRect(s, { left: 60, top: 180, width: 525, height: 440 }, C.bg, C.grid, 14, 1);
  const center = boxWithText(s, "先看效果\n再拆路径", { left: 238, top: 326, width: 170, height: 110 }, {
    fill: C.dark, lineFill: C.dark, color: C.paper, fontSize: 21, radius: 55,
  });
  const tocNodes = [
    ["01", "系统全景", 95, 225],
    ["02", "代码边界", 395, 225],
    ["03", "执行机制", 95, 493],
    ["04", "运行与观察", 395, 493],
  ];
  const nodeShapes = tocNodes.map(([num, label, x, y], i) => {
    const n = boxWithText(s, `${num}\n${label}`, { left: x, top: y, width: 130, height: 80 }, {
      fill: i === 0 ? C.redPale : C.paper,
      lineFill: i === 0 ? C.red : C.border,
      lineWidth: i === 0 ? 2 : 1,
      color: i === 0 ? C.red : C.ink,
      fontSize: 18,
      typeface: FONT,
    });
    connect(s, n, center, { fromSide: x < 250 ? "right" : "left", toSide: x < 250 ? "left" : "right", color: C.border, width: 1.5, arrow: false });
    return n;
  });
  panel.sendToBack();
  center.bringToFront();
  nodeShapes.forEach(n => n.bringToFront());

  const items = [
    ["01", "系统全景", "宿主、两套 guest 与 UB 数据路径"],
    ["02", "代码边界", "官方 provider 与 gem5 设备模型如何分工"],
    ["03", "执行机制", "一次 SEND_IMM 如何跨节点走完"],
    ["04", "运行与观察", "如何复现，以及结果如何解释"],
  ];
  items.forEach(([num, title, sub], i) => {
    const y = 197 + i * 105;
    textBox(s, num, { left: 635, top: y + 7, width: 48, height: 30 }, {
      typeface: MONO, fontSize: 16, bold: true, color: i === 0 ? C.red : C.grayBlue,
    });
    textBox(s, title, { left: 700, top: y, width: 410, height: 38 }, {
      fontSize: 25, bold: true, color: i === 0 ? C.red : C.ink, autoFit: "shrinkText",
    });
    textBox(s, sub, { left: 700, top: y + 43, width: 440, height: 28 }, {
      fontSize: 16, color: C.body, autoFit: "shrinkText",
    });
    textBox(s, String(i + 1).padStart(2, "0"), { left: 1160, top: y + 12, width: 50, height: 24 }, {
      typeface: MONO, fontSize: 12, color: i === 0 ? C.red : C.grayBlue, alignment: "right",
    });
    if (i < items.length - 1) line(s, 635, y + 86, 575, 0, C.grid, 1);
  });
  footer(s, 2);
  addNotes(s,
    "目录按技术分享的讲解顺序组织：先看实验环境与运行现象，再拆官方代码和仿真设备的边界，随后解释跨节点执行与虚拟时间，最后给出复现方法和结果解读。\n\n" +
    "规划文件：" + path.join(PRESENTATION_DIR, "OpenURMA_gem5_AICO.plan.md")
  );
}

// 3. Target implementation stack
{
  const s = deck.slides.add();
  header(s, "1.1 · 系统全景 / SETUP", "一台 Apple Silicon Mac 承载两套可交互的 ARM64 UB 客机", 3);

  function guestStack(x, node, role, consolePort, eid, oobIp) {
    const outer = roundRect(s, { left: x, top: 170, width: 520, height: 408 }, C.paper, C.grayBlue, 14, 1.5);
    shape(s, "rect", { left: x + 1, top: 171, width: 518, height: 43 }, C.pale, C.pale, 0);
    textBox(s, `${node} / ${role}`, { left: x + 18, top: 181, width: 210, height: 24 }, {
      typeface: MONO, fontSize: 15, bold: true, color: C.ink, wrap: "none",
    });
    textBox(s, `gem5 24.0.0.1 · PL011 ${consolePort}`, { left: x + 240, top: 184, width: 260, height: 20 }, {
      typeface: MONO, fontSize: 11, color: C.grayBlue, alignment: "right", autoFit: "shrinkText", wrap: "none",
    });

    const app = boxWithText(s, `urma_perftest send_lat  /  ${role.toLowerCase()}\n${eid}  ·  OOB ${oobIp}`, {
      left: x + 20, top: 226, width: 480, height: 54,
    }, { fill: C.pale, lineFill: C.grayBlue, color: C.ink, fontSize: 15, lineWidth: 1.2 });
    const provider = boxWithText(s, "openEuler UMDK 25.12.0\nliburma + liburma-udma.so", {
      left: x + 20, top: 288, width: 480, height: 48,
    }, { fill: C.paper, lineFill: C.border, color: C.ink, fontSize: 15 });
    const guest = boxWithText(s, "openEuler OLK 6.6 ARM64 kernel  +  BusyBox initramfs", {
      left: x + 20, top: 344, width: 480, height: 42,
    }, { fill: C.paper, lineFill: C.border, color: C.ink, fontSize: 14 });
    const officialKmod = boxWithText(s, "uburma.ko + ubcore.ko\n官方内核模块", {
      left: x + 20, top: 394, width: 285, height: 52,
    }, { fill: C.paper, lineFill: C.border, color: C.ink, fontSize: 14 });
    const addedKmod = boxWithText(s, "openurma_ubcore.ko\n注册 openurma0", {
      left: x + 313, top: 394, width: 187, height: 52,
    }, { fill: C.redPale, lineFill: C.red, color: C.red, fontSize: 14, lineWidth: 1.8 });
    const cpu = boxWithText(s, "ArmAtomicSimpleCPU\n1 core · 3 GHz · no CPU cache", {
      left: x + 20, top: 454, width: 294, height: 54,
    }, { fill: C.dark, lineFill: C.dark, color: C.paper, typeface: MONO, fontSize: 13, radius: 8 });
    const memory = boxWithText(s, "1 GB DDR3-1600\n1 channel", {
      left: x + 322, top: 454, width: 178, height: 54,
    }, { fill: C.pale, lineFill: C.border, color: C.ink, typeface: MONO, fontSize: 13, radius: 8 });
    const nic = boxWithText(s, "NICTopologySC UDMA NIC · Doorbell / DMA / CQE", {
      left: x + 20, top: 516, width: 480, height: 42,
    }, { fill: C.redPale, lineFill: C.red, color: C.red, typeface: MONO, fontSize: 13, lineWidth: 1.8, radius: 8 });
    outer.sendToBack();
    return { app, provider, guest, officialKmod, addedKmod, cpu, memory, nic };
  }

  const node0 = guestStack(60, "NODE 0", "SERVER", "3460", "EID fe80::1", "10.0.0.1");
  const node1 = guestStack(700, "NODE 1", "CLIENT", "3470", "EID fe80::2", "10.0.0.2");

  connect(s, node0.app, node1.app, { color: C.grayBlue, width: 1.5, dashed: true, bidirectional: true });
  const oob = boxWithText(s, "OOB TCP\nsetup / control", { left: 590, top: 232, width: 100, height: 42 }, {
    fill: C.paper, lineFill: C.border, color: C.grayBlue, typeface: MONO, fontSize: 11, radius: 20,
  });
  oob.bringToFront();

  connect(s, node0.nic, node1.nic, { color: C.red, width: 3.5, bidirectional: true });
  const ub = boxWithText(s, "UB DATA\n400G · 100 ns", { left: 586, top: 510, width: 108, height: 54 }, {
    fill: C.dark, lineFill: C.dark, color: C.paper, typeface: MONO, fontSize: 12, radius: 27,
  });
  ub.bringToFront();

  line(s, 320, 578, 0, 23, C.grayBlue, 1.5);
  line(s, 960, 578, 0, 23, C.grayBlue, 1.5);
  const host = roundRect(s, { left: 60, top: 601, width: 1160, height: 66 }, C.dark, C.dark, 10, 0);
  textBox(s, "macOS HOST", { left: 82, top: 618, width: 140, height: 22 }, {
    typeface: MONO, fontSize: 14, bold: true, color: C.paper, wrap: "none",
  });
  textBox(s, "MacBook Air · Apple M4 · 16 GB", { left: 222, top: 617, width: 280, height: 24 }, {
    typeface: MONO, fontSize: 13, color: "#E5E8ED", autoFit: "shrinkText", wrap: "none",
  });
  textBox(s, "Docker Desktop Linux VM · aarch64 Ubuntu 22.04 container", { left: 500, top: 617, width: 435, height: 24 }, {
    typeface: MONO, fontSize: 12, color: "#C7CDD8", autoFit: "shrinkText", wrap: "none",
  });
  textBox(s, "openurma-repro-20260909", { left: 940, top: 617, width: 255, height: 24 }, {
    typeface: MONO, fontSize: 12, bold: true, color: "#F0C3C6", alignment: "right", autoFit: "shrinkText", wrap: "none",
  });
  textBox(s, "100 ns distributed-time synchronization runs beside the two guests; it is not a UB switch", {
    left: 500, top: 643, width: 695, height: 15,
  }, { typeface: MONO, fontSize: 9.5, color: "#9FA8B6", alignment: "right", autoFit: "shrinkText", wrap: "none" });
  host.sendToBack();

  addNotes(s,
    "这页先建立运行层次。最底层是当前 macOS / Apple Silicon 宿主；Docker Desktop 中的 ARM64 Ubuntu 22.04 容器同时运行两个独立 gem5 24.0.0.1 进程。每个 gem5 进程包含一套可登录的 ARM64 full-system guest。\n\n" +
    "guest 不是完整 openEuler 用户态发行版：它使用 openEuler OLK 6.6 ARM64 内核与 BusyBox initramfs。用户态运行基于官方源码适配的 urma_perftest，UDMA provider 子目录保持原样。官方 ubcore.ko 与 uburma.ko 继续运行，openurma_ubcore.ko 和 NICTopologySC 补齐设备注册与硬件行为。\n\n" +
    "本页对齐当前 run-dual 快速配置：每节点 1 个 ArmAtomicSimpleCPU、3 GHz、无 CPU cache、1 GB DDR3-1600。manifest 虽保留 cache 参数，但 cpu_mode=atomic 时配置不会实例化 cache。红线是 400 Gbit/s、100 ns 的 UB 数据路径；灰色虚线是只负责建连和资源交换的 Ethernet OOB。\n\n" +
    "来源：" + path.join(LAB, "run-dual/run-manifest.txt") + "；" +
    path.join(LAB, "run-dual/node0/gem5.log") + "；" +
    path.join(LAB, "README.md") + ":1-124。"
  );
}

// 4. Working demo
{
  const s = deck.slides.add();
  header(s, "1.2 · 先看效果 / DEMO", "当前原型已经完成一次双节点 CTP/RM SEND_IMM", 4);

  const stats = [
    ["2", "独立 ARM64 Linux 节点", "分别启动、分别接入串口"],
    ["ACTIVE", "openurma0 端口状态", "本地管理面报告端口 ACTIVE"],
    ["128 B", "CTP/RM SEND_IMM", "服务端与客户端完成 20 次采样"],
  ];
  stats.forEach(([value, label, sub], i) => {
    const x = 60 + i * 395;
    textBox(s, value, { left: x, top: 170, width: 355, height: 68 }, {
      typeface: MONO, fontSize: i === 1 ? 43 : 50, bold: true, color: C.red,
      alignment: "center", verticalAlignment: "middle", autoFit: "shrinkText",
    });
    textBox(s, label, { left: x, top: 244, width: 355, height: 30 }, {
      fontSize: 20, bold: true, color: C.ink, alignment: "center", autoFit: "shrinkText",
    });
    textBox(s, sub, { left: x + 15, top: 282, width: 325, height: 44 }, {
      fontSize: 16, color: C.body, alignment: "center", autoFit: "shrinkText",
    });
    if (i < 2) line(s, x + 374, 180, 0, 136, C.grid, 1);
  });

  labelText(s, "WHAT HAPPENS", { left: 60, top: 365, width: 280, height: 20 });
  const steps = [
    ["启动两个 guest", "Linux + initramfs"],
    ["加载基础模块", "ubcore / uburma"],
    ["注册仿真设备", "openurma0 / ABI=udma"],
    ["两端完成 send_lat", "CTP/RM SEND_IMM"],
  ];
  const nodes = steps.map(([title, sub], i) => boxWithText(s, `${title}\n${sub}`, {
    left: 60 + i * 293, top: 402, width: 250, height: 98,
  }, {
    fill: i === 2 ? C.redPale : C.paper,
    lineFill: i === 2 ? C.red : C.border,
    lineWidth: i === 2 ? 2 : 1,
    color: i === 2 ? C.red : C.ink,
    fontSize: 18,
  }));
  for (let i = 0; i < nodes.length - 1; i++) connect(s, nodes[i], nodes[i + 1], { color: C.red2, width: 2 });
  textBox(s, "后续页面沿着这条路径拆解：谁写 WQE，谁消费 Doorbell，谁完成 DMA 与 CQE。", {
    left: 60, top: 548, width: 1160, height: 46,
  }, { fontSize: 20, color: C.body, alignment: "center", autoFit: "shrinkText" });
  addNotes(s,
    "先展示运行现象，再进入机制。两个节点分别进入 Linux；urma_admin 能枚举 openurma0、eid0，并显示本地端口 ACTIVE；官方 provider 参与 WQE/CQE 热路径，整套系统完成 SEND_IMM 双端闭环。ACTIVE 只表示本地管理面状态，peer 连通要看双端 send_lat 是否真正完成。\n\n" +
    "运行证据见用户提供的三张截图；当前 profile 见：" + path.join(LAB, "run-dual/run-manifest.txt")
  );
}

// 5. Code boundary
{
  const s = deck.slides.add();
  header(s, "2.1 · 代码边界 / BOUNDARY", "官方代码保持原有职责，gem5 补齐缺失的设备侧行为", 5);
  const values = [
    ["来源", "组件", "代码处理", "在请求路径中的作用"],
    ["官方原样", "ubcore.ko + uburma.ko", "原样运行", "UB 核心与用户态 ioctl / mmap 通道"],
    ["官方原样", "liburma-udma.so", "原样运行", "创建资源、编码 WQE、写 Doorbell、轮询 CQE"],
    ["官方原样", "urma_admin", "原样运行", "枚举设备、EID 与 link 状态"],
    ["官方基础 + 适配", "urma_perftest", "加入仿真适配", "保留 benchmark 主体，补 gem5 时钟、双端同步、ROI 与统计"],
    ["本工程新增", "openurma_ubcore.ko", "新增 bridge", "实现 ubcore device ops，注册 openurma0 并下发资源描述"],
    ["本工程新增", "NICTopologySC + UdmaSimAbi", "新增设备模型", "消费 Doorbell，DMA 读写 SQ/RQ/CQ，跨节点投递"],
    ["本工程新增", "libummu.so.1 shim", "新增仿真 shim", "满足本次映射与记账，不等同完整 UMMU"],
    ["本工程新增", "initramfs / run / sync / attach", "新增运行脚本", "构建镜像，启动与同步双节点，接入两个串口"],
  ];
  const table = s.tables.add({
    rows: values.length,
    columns: 4,
    left: 60,
    top: 164,
    width: 1160,
    height: 450,
    columnWidths: [175, 270, 165, 550],
    values,
  });
  table.borders.assign({ style: "solid", fill: "#000000", width: 1 });
  table.styleOptions = { headerRow: true, bandedRows: false };
  for (let r = 0; r < values.length; r++) {
    table.rows[r].height = r === 0 ? 46 : 50.5;
    for (let c = 0; c < 4; c++) {
      const cell = table.getCell(r, c);
      cell.fill = r === 0 ? C.red : C.paper;
      cell.text.style = {
        typeface: c === 1 ? MONO : FONT,
        fontSize: r === 0 ? 16 : (c === 3 ? 15 : 16),
        bold: r === 0 || c === 0,
        color: r === 0 ? C.paper : C.ink,
        verticalAlignment: "middle",
        alignment: c === 0 || c === 2 ? "center" : "left",
        autoFit: "shrinkText",
      };
    }
    if (r > 0) {
      const kind = values[r][0];
      const color = kind === "官方原样" ? C.ink : (kind.includes("适配") ? C.grayBlue : C.red);
      table.getCell(r, 0).text.style = {
        typeface: FONT, fontSize: 15, bold: true, color,
        alignment: "center", verticalAlignment: "middle", autoFit: "shrinkText",
      };
    }
  }
  shape(s, "rect", { left: 60, top: 632, width: 5, height: 31 }, C.red, C.red, 0);
  textBox(s, "关键边界：CONFIG_UB_UDMA 关闭；本次没有加载官方 kernel UDMA 硬件驱动，硬件行为由仿真设备承接。", {
    left: 79, top: 634, width: 1141, height: 28,
  }, { fontSize: 16, bold: true, color: C.ink, autoFit: "shrinkText" });
  addNotes(s,
    "这页是软件归属的权威口径。可以称为官方原样运行的是 ubcore、uburma、urma_admin 和 UDMA userspace provider。urma_perftest 的主体来自官方 UMDK，但加入了仿真时间与统计适配。其余 bridge、仿真 NIC、同步和运行包装是本工程新增。\n\n" +
    "来源：\n" +
    path.join(LAB, "build_olk66.sh") + ":70-85（CONFIG_UB_UDMA disabled）\n" +
    path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/hw/udma") + "（官方 provider，工作树 clean）\n" +
    path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/tools/urma_perftest") + "（本地适配）\n" +
    path.join(OPENURMA, "integration/umdk/kmod/openurma_ubcore.c") + "（新增）\n" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/UdmaSimAbi.hh") + " 与 NICTopologySC.cc（新增）"
  );
}

// 6. Provider and simulated data path
{
  const s = deck.slides.add();
  header(s, "2.2 · 请求路径 / CODE PATH", "一次 SEND_IMM 从官方 provider 的 WQE 走到对端 CQE", 6);

  const officialCode = `for (it = wr; it != NULL; it = it->next) {\n  ret = udma_u_post_one_wr(...);\n  wr_cnt++;\n}\nif (wr_cnt) {\n  UDMA_TO_DEVICE_BARRIER();\n  udma_update_sq_db(sq);\n}\n*db_addr = sq->pi;`;
  const simCode = `if (off == doorbell) {\n  producer = loadLe32(data);\n  udma_enqueue_sq(jetty.id, producer);\n}\nudma_dma_ring(... sq_va ..., false);\n// decode WQE and transfer payload\nudma_dma_ring(... cq_va ..., true);`;
  codeBlock(s, "OFFICIAL / udma_u_jfs.c + .h", officialCode, { left: 60, top: 162, width: 486, height: 218 }, { fontSize: 14 });
  codeBlock(s, "ADDED / NICTopologySC.cc", simCode, { left: 60, top: 395, width: 486, height: 218 }, { accent: C.grayBlue, titleColor: "#AEB7C5", fontSize: 14 });

  labelText(s, "OFFICIAL USERSPACE", { left: 590, top: 163, width: 240, height: 20 });
  const p1 = boxWithText(s, "编码 WQE", { left: 590, top: 205, width: 160, height: 64 }, { fill: C.paper, lineFill: C.border, fontSize: 18 });
  const p2 = boxWithText(s, "内存屏障", { left: 788, top: 205, width: 160, height: 64 }, { fill: C.paper, lineFill: C.border, fontSize: 18 });
  const p3 = boxWithText(s, "写 Doorbell", { left: 986, top: 205, width: 160, height: 64 }, { fill: C.paper, lineFill: C.border, fontSize: 18 });
  [p1, p2, p3].forEach((n) => n.bringToFront());
  connect(s, p1, p2, { color: C.grayBlue, width: 2 });
  connect(s, p2, p3, { color: C.grayBlue, width: 2 });
  labelText(s, "ADDED GEM5 UDMA", { left: 590, top: 315, width: 240, height: 20 });

  const g1 = boxWithText(s, "捕获 Doorbell", { left: 986, top: 342, width: 160, height: 64 }, { fill: C.redPale, lineFill: C.red, color: C.red, fontSize: 18, lineWidth: 2 });
  const g2 = boxWithText(s, "DMA 读 SQ\n与 payload", { left: 788, top: 342, width: 160, height: 64 }, { fill: C.redPale, lineFill: C.red, color: C.red, fontSize: 17, lineWidth: 2 });
  const g3 = boxWithText(s, "peer 投递", { left: 590, top: 342, width: 160, height: 64 }, { fill: C.redPale, lineFill: C.red, color: C.red, fontSize: 18, lineWidth: 2 });
  connect(s, p3, g1, { kind: "straight", fromSide: "bottom", toSide: "top", color: C.red2, width: 2 });
  connect(s, g1, g2, { fromSide: "left", toSide: "right", color: C.red, width: 2 });
  connect(s, g2, g3, { fromSide: "left", toSide: "right", color: C.red, width: 2 });

  const cqe = boxWithText(s, "CQE 回写", { left: 590, top: 478, width: 160, height: 64 }, { fill: C.redPale, lineFill: C.red, color: C.red, fontSize: 18, lineWidth: 2 });
  const poll = boxWithText(s, "轮询完成", { left: 788, top: 478, width: 160, height: 64 }, { fill: C.paper, lineFill: C.border, color: C.ink, fontSize: 18 });
  connect(s, g3, cqe, { kind: "straight", fromSide: "bottom", toSide: "top", color: C.red, width: 2 });
  connect(s, cqe, poll, { color: C.grayBlue, width: 2 });
  textBox(s, "官方 provider 读取 completion", { left: 788, top: 555, width: 358, height: 24 }, {
    fontSize: 14, color: C.body, alignment: "center",
  });
  textBox(s, "CPU 负责 WQE 语义与队列推进；缺失的设备侧消费、DMA 和跨节点传输由 gem5 接管。", {
    left: 590, top: 614, width: 556, height: 35,
  }, { fontSize: 17, color: C.ink, bold: true, alignment: "center", autoFit: "shrinkText" });
  addNotes(s,
    "左上是官方 provider 的真实热路径：逐个编码 WR、执行 device barrier，然后写 SQ doorbell。右侧不是伪造 API 返回，而是仿真设备捕获 doorbell、DMA 读取客机内存中的 SQ/WQE 和 payload，再向 peer 投递并写回 CQE。官方 provider 最后仍通过原有 CQ polling 得到完成。\n\n" +
    "来源：\n" +
    path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/hw/udma/udma_u_jfs.c") + ":821-837\n" +
    path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/hw/udma/udma_u_jfs.h") + ":129-133\n" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/NICTopologySC.cc") + ":2402-2421、1440-1457、1504-1523、1278-1287"
  );
}

// 7. Virtual time and link timing
{
  const s = deck.slides.add();
  header(s, "3.1 · 虚拟时间 / TIMING", "100 ns 同步量子约束双进程因果推进，链路延迟来自传输机制", 7);
  shape(s, "rect", { left: 60, top: 164, width: 780, height: 56 }, C.dark, C.dark, 0);
  textBox(s, "t_delivery = t_send + payload_bits / 400 Gbit/s + 100 ns", { left: 83, top: 178, width: 734, height: 30 }, {
    typeface: MONO, fontSize: 20, bold: true, color: C.paper, alignment: "center", autoFit: "shrinkText",
  });
  textBox(s, "* 当前空闲链路 / 单流近似；FIFO 排队由事件链另行体现", { left: 60, top: 224, width: 780, height: 16 }, {
    fontSize: 10, color: C.grayBlue, alignment: "right", autoFit: "shrinkText",
  });

  labelText(s, "CONSERVATIVE ADVANCE", { left: 60, top: 246, width: 280, height: 20 });
  const laneY0 = 300;
  const laneY1 = 425;
  textBox(s, "NODE 0", { left: 60, top: laneY0 + 15, width: 80, height: 24 }, { typeface: MONO, fontSize: 14, bold: true, color: C.ink });
  textBox(s, "NODE 1", { left: 60, top: laneY1 + 15, width: 80, height: 24 }, { typeface: MONO, fontSize: 14, bold: true, color: C.ink });
  line(s, 145, laneY0 + 34, 695, 0, C.border, 2);
  line(s, 145, laneY1 + 34, 695, 0, C.border, 2);
  const a = boxWithText(s, "doorbell\nt_send", { left: 175, top: laneY0, width: 130, height: 68 }, { fill: C.paper, lineFill: C.border, typeface: MONO, fontSize: 16 });
  const b = boxWithText(s, "计算可交付时刻\n序列化 + 传播", { left: 365, top: laneY0, width: 190, height: 68 }, { fill: C.redPale, lineFill: C.red, color: C.red, fontSize: 16, lineWidth: 2 });
  const c = boxWithText(s, "写入 peer ring", { left: 615, top: laneY0, width: 160, height: 68 }, { fill: C.paper, lineFill: C.border, fontSize: 16 });
  connect(s, a, b, { color: C.red2, width: 2 });
  connect(s, b, c, { color: C.red2, width: 2 });
  const d = boxWithText(s, "同步窗口安全", { left: 365, top: laneY1, width: 190, height: 68 }, { fill: C.redPale, lineFill: C.red, color: C.red, fontSize: 16, lineWidth: 2 });
  const e = boxWithText(s, "到期投递\n更新 RQ / CQ", { left: 615, top: laneY1, width: 160, height: 68 }, { fill: C.paper, lineFill: C.border, fontSize: 16 });
  connect(s, c, d, { kind: "elbow", fromSide: "bottom", toSide: "top", color: C.red, width: 2 });
  connect(s, d, e, { color: C.red, width: 2 });

  for (let i = 0; i < 6; i++) {
    const x = 145 + i * 139;
    line(s, x, 525, 0, 18, i === 0 || i === 5 ? C.red : C.border, i === 0 || i === 5 ? 2 : 1);
    textBox(s, `${i * 100} ns`, { left: x - 30, top: 548, width: 70, height: 18 }, { typeface: MONO, fontSize: 10, color: C.grayBlue, alignment: "center" });
  }
  line(s, 145, 534, 695, 0, C.grayBlue, 1.5);
  textBox(s, "两端只推进到共同可证明安全的虚拟时间边界", { left: 180, top: 586, width: 625, height: 28 }, {
    fontSize: 17, color: C.body, alignment: "center",
  });

  roundRect(s, { left: 885, top: 164, width: 335, height: 468 }, C.paper, C.border, 14, 1);
  labelText(s, "RUN MANIFEST", { left: 910, top: 188, width: 190, height: 20 });
  const params = [
    ["peer link", "400 Gbit/s"],
    ["propagation", "100 ns"],
    ["sync quantum", "100 ns"],
    ["serialization", "1 stage"],
    ["switch delay", "0 ns"],
    ["fixed service add-ons", "0 ns"],
    ["data pipeline", "pipe_data=0"],
  ];
  params.forEach(([k, v], i) => {
    const y = 232 + i * 52;
    textBox(s, k, { left: 910, top: y, width: 165, height: 22 }, { typeface: MONO, fontSize: 12, color: C.body, autoFit: "shrinkText" });
    textBox(s, v, { left: 1068, top: y - 2, width: 125, height: 25 }, {
      typeface: MONO, fontSize: 14, bold: true, color: k === "sync quantum" ? C.red : C.ink, alignment: "right", autoFit: "shrinkText",
    });
    if (i < params.length - 1) line(s, 910, y + 33, 283, 0, C.grid, 1);
  });
  addNotes(s,
    "模型把两个 gem5 进程的推进限制在保守同步窗口内。发送侧基于 payload 大小与 400 Gbit/s 链路计算序列化时间，再叠加 100 ns propagation。peer 事件只有在接收端推进到安全边界后才投递，避免宿主调度快慢改变事件因果顺序。\n\n" +
    "当前没有用固定 service delay 去拟合实测：direct_wqe_latency、sq_fetch_latency、sq_wqebb_latency、payload_dma_latency 均为 0；switch delay 也为 0。\n\n" +
    "来源：" + path.join(LAB, "run-dual/run-manifest.txt") + "；" +
    path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/NICTopologySC.cc") + ":2239-2250、2147-2187。"
  );
}

// 8. Boot and ACTIVE observation
{
  const s = deck.slides.add();
  header(s, "3.2 · 运行观察 / RUNTIME", "启动日志显示设备已注册，urma_admin 显示本地端口 ACTIVE", 8);

  labelText(s, "GUEST BOOT", { left: 60, top: 160, width: 220, height: 20 });
  const bootPos = { left: 60, top: 192, width: 850, height: 274 };
  shape(s, "rect", bootPos, C.code, C.code, 0);
  addImage(s, bootBytes, "image/png", { left: 70, top: 202, width: 830, height: 254 }, "Guest boot log showing ubcore, uburma and openurma_ubcore modules", "contain");
  cornerBrackets(s, bootPos, C.red);

  textBox(s, "日志里能读出什么", { left: 955, top: 184, width: 250, height: 34 }, { fontSize: 21, bold: true, color: C.ink });
  const ev = [
    ["01", "ubcore.ko 与 uburma.ko 已加载"],
    ["02", "openurma_ubcore.ko 已加载"],
    ["03", "openurma0 注册为 UB / ABI=udma"],
  ];
  ev.forEach(([n, t], i) => {
    textBox(s, n, { left: 955, top: 241 + i * 67, width: 42, height: 24 }, { typeface: MONO, fontSize: 15, bold: true, color: C.red });
    textBox(s, t, { left: 1002, top: 236 + i * 67, width: 205, height: 45 }, { fontSize: 16, color: C.ink, bold: true, autoFit: "shrinkText" });
  });

  labelText(s, "URMA_ADMIN SHOW", { left: 60, top: 497, width: 240, height: 20 });
  const activePos = { left: 60, top: 528, width: 1160, height: 102 };
  shape(s, "rect", activePos, C.code, C.code, 0);
  addImage(s, activeBytes, "image/png", { left: 70, top: 536, width: 1140, height: 86 }, "urma_admin output showing openurma0 and ACTIVE link", "contain");
  cornerBrackets(s, activePos, C.red);
  boxWithText(s, "ACTIVE", { left: 1070, top: 486, width: 134, height: 35 }, {
    fill: C.red, lineFill: C.red, color: C.paper, typeface: MONO, fontSize: 17, radius: 18,
    autoFit: "none", insets: { top: 0, right: 0, bottom: 0, left: 0 },
  });
  addNotes(s,
    "启动日志说明模块顺序和设备注册已经完成。urma_admin 的输出来自官方用户态管理工具，它通过正式 API 枚举到 openurma0、eid0，并显示本地端口 ACTIVE。当前驱动向管理面报告 ACTIVE；该状态本身不等价于 peer 已连通，仍需观察双端 send_lat 是否完成。\n\n" +
    "用户截图：\n" + IMG_BOOT + "\n" + IMG_ACTIVE + "\n" +
    "相关源码：" + path.join(OPENURMA, "integration/umdk/kmod/openurma_ubcore.c") + ":1366-1423。"
  );
}

// 9. Reproduction commands
{
  const s = deck.slides.add();
  header(s, "4.1 · 动手复现 / TRY IT", "两端各一条命令启动同一 CTP/RM/SEND_IMM 测试", 9);

  const server = roundRect(s, { left: 60, top: 172, width: 510, height: 330 }, C.code, C.code, 14, 0);
  const client = roundRect(s, { left: 710, top: 172, width: 510, height: 330 }, C.code, C.code, 14, 0);
  shape(s, "rect", { left: 60, top: 172, width: 6, height: 330 }, C.grayBlue, C.grayBlue, 0);
  shape(s, "rect", { left: 714, top: 172, width: 6, height: 330 }, C.red, C.red, 0);
  textBox(s, "NODE 0 / SERVER", { left: 86, top: 194, width: 300, height: 24 }, { typeface: MONO, fontSize: 14, bold: true, color: "#AEB7C5" });
  textBox(s, "ou-lat-server --profile\nctp-rm-send-imm-i128 20 128 21115", { left: 86, top: 244, width: 452, height: 90 }, {
    typeface: MONO, fontSize: 20, bold: true, color: C.paper, autoFit: "shrinkText",
  });
  textBox(s, "创建 Jetty / MR / JFC\n监听控制通道并等待客户端", { left: 86, top: 382, width: 452, height: 68 }, {
    fontSize: 18, color: "#C9CFD8", autoFit: "shrinkText",
  });

  textBox(s, "NODE 1 / CLIENT", { left: 742, top: 194, width: 300, height: 24 }, { typeface: MONO, fontSize: 14, bold: true, color: C.red2 });
  textBox(s, "ou-lat-client --profile\nctp-rm-send-imm-i128 20 128 21115", { left: 742, top: 244, width: 446, height: 90 }, {
    typeface: MONO, fontSize: 20, bold: true, color: C.paper, autoFit: "shrinkText",
  });
  textBox(s, "连接服务端并交换资源\n发起 CTP/RM SEND_IMM", { left: 742, top: 382, width: 446, height: 68 }, {
    fontSize: 18, color: "#C9CFD8", autoFit: "shrinkText",
  });
  connect(s, server, client, { color: C.red, width: 4, bidirectional: true });
  boxWithText(s, "400 Gbit/s\npeer path", { left: 584, top: 285, width: 112, height: 70 }, {
    fill: C.paper, lineFill: C.red, color: C.red, typeface: MONO, fontSize: 15, radius: 35, lineWidth: 2,
  });

  labelText(s, "PROFILE EXPANSION", { left: 60, top: 535, width: 240, height: 20 });
  const profile = [
    ["传输模式", "CTP / RM"],
    ["语义", "SEND_IMM"],
    ["JFS post list", "1"],
    ["inline", "128 B"],
    ["measured", "20"],
  ];
  profile.forEach(([k, v], i) => {
    const x = 60 + i * 232;
    textBox(s, k, { left: x, top: 574, width: 205, height: 22 }, { typeface: MONO, fontSize: 12, color: C.body, alignment: "center", autoFit: "shrinkText" });
    textBox(s, v, { left: x, top: 606, width: 205, height: 28 }, { typeface: MONO, fontSize: 17, bold: true, color: i === 1 ? C.red : C.ink, alignment: "center", autoFit: "shrinkText" });
    if (i < profile.length - 1) line(s, x + 220, 570, 0, 69, C.grid, 1);
  });
  textBox(s, "包装脚本只固定双端角色与参数；底层运行基于官方源码适配的 urma_perftest，UDMA provider 子目录保持原样。", { left: 60, top: 653, width: 1160, height: 24 }, {
    fontSize: 15, color: C.body, alignment: "center", autoFit: "shrinkText",
  });
  addNotes(s,
    "send_lat 需要服务端和客户端各运行一个进程，因为两端先交换地址、EID、Jetty 等资源信息，再进入数据测量。包装命令统一了 CTP/RM/SEND_IMM、JFS post list、inline 与采样参数；底层运行基于官方源码适配的 urma_perftest，UDMA provider 子目录未改。\n\n" +
    "相关文件：" + path.join(LAB, "overlay/usr/local/bin/ou-lat-server") + "、" +
    path.join(LAB, "overlay/usr/local/bin/ou-lat-client") + "、" +
    path.join(OPENURMA, "integration/umdk/vendor/umdk/src/urma/tools/urma_perftest")
  );
}

// 10. Result interpretation
{
  const s = deck.slides.add();
  header(s, "4.2 · 结果解读 / OBSERVATION", "为什么 20 个样本下 p99 会等于最大值", 10);
  const resultPos = { left: 60, top: 160, width: 1160, height: 215 };
  shape(s, "rect", resultPos, C.code, C.code, 0);
  addImage(s, resultBytes, "image/png", { left: 70, top: 168, width: 1140, height: 199 }, "128-byte send_lat result with 20 samples and latency statistics", "contain");
  cornerBrackets(s, resultPos, C.red);
  labelText(s, "NODE 0 / SERVER OUTPUT", { left: 60, top: 380, width: 250, height: 18 });

  const metrics = [
    ["44.03 µs", "min / median"],
    ["52.15 µs", "average"],
    ["35.16 µs", "stdev"],
    ["205.42 µs", "max / p99"],
  ];
  metrics.forEach(([v, l], i) => metric(s, v, l, { left: 60 + i * 293, top: 402, width: 265, height: 92 }, {
    valueColor: i === 3 ? C.red : C.ink,
    fill: i === 3 ? C.redPale : C.paper,
    lineFill: i === 3 ? C.red : C.border,
    valueSize: 28,
  }));

  roundRect(s, { left: 60, top: 521, width: 490, height: 112 }, C.redPale, C.red, 12, 1.5);
  labelText(s, "WHY P99 = MAX", { left: 82, top: 540, width: 200, height: 20 });
  textBox(s, "ceil(0.99 × 20) = 20", { left: 82, top: 570, width: 430, height: 30 }, {
    typeface: MONO, fontSize: 18, bold: true, color: C.red, autoFit: "shrinkText",
  });
  textBox(s, "nearest-rank 取第 20 个样本，因此当前 p99 与最大值相同。", { left: 82, top: 607, width: 430, height: 20 }, {
    fontSize: 13, color: C.ink, autoFit: "shrinkText",
  });

  roundRect(s, { left: 575, top: 521, width: 645, height: 112 }, C.paper, C.border, 12, 1);
  labelText(s, "FOLLOW-UP EXPERIMENTS", { left: 598, top: 540, width: 220, height: 20 }, C.grayBlue);
  const next = [
    ["01", "样本扩到 1001+，再讨论尾延迟"],
    ["02", "按 2 B 到 4096 B 扫描包长趋势"],
    ["03", "直连与 L1 交换拓扑分别比较"],
  ];
  next.forEach(([n, t], i) => {
    textBox(s, n, { left: 598 + i * 200, top: 575, width: 34, height: 22 }, { typeface: MONO, fontSize: 13, bold: true, color: C.red });
    textBox(s, t, { left: 635 + i * 200, top: 567, width: 158, height: 49 }, { fontSize: 14, color: C.ink, autoFit: "shrinkText" });
    if (i < 2) line(s, 792 + i * 200, 568, 0, 49, C.grid, 1);
  });
  addNotes(s,
    "本页指标来自截图中的 Node 0/server 输出，不泛化为另一端数据。这组 20 次结果已经证明 WQE、doorbell、DMA、peer 和 CQE 的时间链能够闭合。nearest-rank 定义下，20 个样本的 p99 排名是第 20 个，所以它必然等于最大值；当前数据不用于估计稳定尾分位。\n\n" +
    "下一步先把样本数提高到至少 1001，再做 2 B 到 4096 B 包长扫描。直连拓扑与经过 L1 交换的实测需要分开看趋势，不把两种物理结构做绝对值拟合。\n\n" +
    "用户截图：" + IMG_RESULT
  );
}

// 11. Thanks
{
  const s = deck.slides.add();
  s.background.fill = C.paper;
  shape(s, "rect", { left: 0, top: 0, width: 12, height: 720 }, C.red, C.red, 0);
  textBox(s, "Thank you.", { left: 78, top: 170, width: 640, height: 120 }, {
    fontSize: 76, bold: false, color: C.ink, verticalAlignment: "middle",
  });
  line(s, 82, 326, 128, 0, C.red, 4);
  textBox(s, "OpenURMA × gem5 双节点全系统仿真", { left: 82, top: 360, width: 560, height: 44 }, {
    fontSize: 24, bold: true, color: C.ink, autoFit: "shrinkText",
  });
  textBox(s, "Questions & Discussion", { left: 82, top: 424, width: 360, height: 30 }, {
    typeface: MONO, fontSize: 17, color: C.grayBlue,
  });
  roundRect(s, { left: 800, top: 170, width: 340, height: 340 }, C.bg, C.grid, 170, 1);
  textBox(s, "UB", { left: 855, top: 244, width: 230, height: 110 }, {
    typeface: MONO, fontSize: 78, bold: true, color: C.red, alignment: "center", verticalAlignment: "middle",
  });
  textBox(s, "FULL-SYSTEM\nDUAL-NODE", { left: 855, top: 356, width: 230, height: 72 }, {
    typeface: MONO, fontSize: 18, bold: true, color: C.ink, alignment: "center", verticalAlignment: "middle",
  });
  textBox(s, "2026.09", { left: 82, top: 650, width: 160, height: 22 }, { typeface: MONO, fontSize: 13, color: C.body });
  addNotes(s, "收尾仅保留项目名称与 Q&A，不重复总结。技术边界、运行命令与结果解释都在前页。\n\n视觉结构参考 AICO-PPT 技术分享模板结语页，但未沿用企业 Logo、使命宣言或免责声明。")
}

const requirements = {
  explicitTotalSlideCount: 11,
  requiredNativeTableOwnerSlides: [5],
  requiredNativeChartOwnerSlides: [],
};
const fontPolicy = { basis: "design", families: [FONT, MONO] };
const stagingDir = path.join(BUILD_DIR, `.codex-finalizer-${VERSION}`);
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
    "--validate-heading-fit",
    "--require-native-table-slide", "5",
  ],
  requiredNativeTableOwnerSlides: [5],
  requiredNativeChartOwnerSlides: [],
  fontPolicy,
  verifyArtifactToolImport: true,
  receiptPath: path.join(stagingDir, `${path.basename(FINAL_PPTX)}.validation.json`),
});

console.log(JSON.stringify({ finalPath: FINAL_PPTX, result }, null, 2));

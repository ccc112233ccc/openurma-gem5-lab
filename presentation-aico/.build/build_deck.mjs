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
const IMG_READ_RESULT = path.join(LAB, "evidence/official-rma-2026-09-19/01-command-and-result.png");
const IMG_RESOURCES = path.join(LAB, "evidence/official-rma-2026-09-19/02-official-resources.png");
const IMG_PORT_PACKETS = path.join(LAB, "evidence/official-rma-2026-09-19/03-dual-port-packets.png");

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

const [coverBytes, coverGeomBytes, readResultBytes, resourceBytes, portPacketBytes] = await Promise.all([
  fs.readFile(IMG_COVER),
  fs.readFile(IMG_COVER_GEOM),
  fs.readFile(IMG_READ_RESULT),
  fs.readFile(IMG_RESOURCES),
  fs.readFile(IMG_PORT_PACKETS),
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
  addImage(s, coverBytes, "image/jpeg", { left: 0, top: 0, width: 1280, height: 720 }, "dark technical texture", "cover");
  shape(s, "rect", { left: 0, top: 0, width: 1280, height: 720 }, "#10141B", "#10141B", 0).opacity = 0.22;
  addImage(s, coverGeomBytes, "image/png", { left: 700, top: 24, width: 540, height: 495 }, "geometric network artwork", "contain");
  shape(s, "rect", { left: 72, top: 76, width: 42, height: 5 }, C.red, C.red, 0);
  textBox(s, "OPENURMA × GEM5 / TECHNICAL SHARING", { left: 128, top: 67, width: 500, height: 24 }, {
    typeface: MONO, fontSize: 14, bold: true, color: "#E6B9BD", wrap: "none",
  });
  textBox(s, "让官方 UB 全栈", { left: 72, top: 160, width: 650, height: 78 }, {
    fontSize: 54, bold: true, color: C.paper, verticalAlignment: "middle",
  });
  textBox(s, "跑进 gem5", { left: 72, top: 238, width: 610, height: 78 }, {
    fontSize: 58, bold: true, color: C.paper, verticalAlignment: "middle",
  });
  textBox(s, "设备发现、UMMU/UDMA、双端口聚合与双节点 RMA", { left: 76, top: 344, width: 700, height: 42 }, {
    fontSize: 22, color: "#D7DCE5", autoFit: "shrinkText",
  });
  const n0 = boxWithText(s, "gem5 node 0\nOLK 6.6 + UMDK", { left: 735, top: 510, width: 190, height: 78 }, {
    fill: "#171B22", lineFill: "#A6AFBB", color: C.paper, typeface: MONO, fontSize: 14, radius: 9,
  });
  const n1 = boxWithText(s, "gem5 node 1\nOLK 6.6 + UMDK", { left: 1020, top: 510, width: 190, height: 78 }, {
    fill: "#171B22", lineFill: C.red2, color: C.paper, typeface: MONO, fontSize: 14, radius: 9, lineWidth: 2,
  });
  connect(s, n0, n1, { color: C.red2, width: 3, bidirectional: true });
  boxWithText(s, "2 × 400G", { left: 936, top: 528, width: 74, height: 38 }, {
    fill: C.red, lineFill: C.red, color: C.paper, typeface: MONO, fontSize: 11, radius: 19,
  }).bringToFront();
  line(s, 76, 608, 570, 0, "#737E8D", 1);
  textBox(s, "技术分享 · 2026.09", { left: 76, top: 628, width: 260, height: 24 }, { typeface: MONO, fontSize: 14, color: C.paper });
  textBox(s, "10 OFFICIAL KERNEL MODULES  /  DUAL PLANE  /  SEND + READ + WRITE", { left: 620, top: 628, width: 590, height: 22 }, {
    typeface: MONO, fontSize: 11, color: "#AEB7C5", alignment: "right", autoFit: "shrinkText", wrap: "none",
  });
  addNotes(s, "新版技术分享以当前官方驱动全栈为主线。核心问题是：保留官方驱动和 provider 的真实职责，只在 gem5 中补齐硬件行为，最终让两个独立全系统节点完成 SEND、READ、WRITE 和 UB 聚合。\n\n视觉沿用现有 AICO 风格，不使用企业 Logo、密级或免责声明。")
}

// 2. System architecture
{
  const s = deck.slides.add();
  header(s, "1 · 系统全景 / ARCHITECTURE", "一台 Apple Silicon Mac 运行两个独立 UB 全系统节点", 2);

  function hostNode(x, node, port, ip) {
    const frame = roundRect(s, { left: x, top: 167, width: 500, height: 400 }, C.paper, C.grayBlue, 12, 1.4);
    textBox(s, node, { left: x + 18, top: 180, width: 210, height: 25 }, { typeface: MONO, fontSize: 16, bold: true, color: C.ink });
    textBox(s, `PL011 ${port} · OOB ${ip}`, { left: x + 245, top: 183, width: 235, height: 20 }, { typeface: MONO, fontSize: 11, color: C.grayBlue, alignment: "right" });
    const app = boxWithText(s, "urma_perftest / urma_admin / ubagg_cli", { left: x + 18, top: 220, width: 464, height: 46 }, { fill: C.pale, lineFill: C.border, fontSize: 15 });
    const umdk = boxWithText(s, "官方 UMDK\nliburma + UDMA provider + UBAGG provider", { left: x + 18, top: 275, width: 464, height: 55 }, { fill: C.paper, lineFill: C.border, fontSize: 15 });
    const kernel = boxWithText(s, "openEuler OLK 6.6 ARM64\n10 个官方 UB 内核模块", { left: x + 18, top: 339, width: 464, height: 57 }, { fill: C.paper, lineFill: C.border, fontSize: 15 });
    const cpu = boxWithText(s, "AtomicSimpleCPU · 1 core · 3 GHz\n1 GB DDR3-1600", { left: x + 18, top: 405, width: 220, height: 62 }, { fill: C.dark, lineFill: C.dark, color: C.paper, typeface: MONO, fontSize: 13, radius: 8 });
    const nic = boxWithText(s, "NICTopologySC\nUBASE · UMMU · UDMA", { left: x + 247, top: 405, width: 235, height: 62 }, { fill: C.redPale, lineFill: C.red, color: C.red, typeface: MONO, fontSize: 13, lineWidth: 1.8, radius: 8 });
    const ports = boxWithText(s, "port 0    port 1\n400G       400G", { left: x + 18, top: 478, width: 464, height: 66 }, { fill: C.paper, lineFill: C.red, color: C.ink, typeface: MONO, fontSize: 14, lineWidth: 1.6 });
    frame.sendToBack();
    return { app, nic, ports };
  }
  const h0 = hostNode(60, "NODE 0 / SERVER", "3460", "10.0.0.1");
  const h1 = hostNode(720, "NODE 1 / CLIENT", "3470", "10.0.0.2");
  connect(s, h0.app, h1.app, { color: C.grayBlue, width: 1.4, dashed: true, bidirectional: true });
  boxWithText(s, "TCP OOB", { left: 590, top: 224, width: 100, height: 36 }, { fill: C.paper, lineFill: C.border, color: C.grayBlue, typeface: MONO, fontSize: 11, radius: 18 }).bringToFront();
  connect(s, h0.ports, h1.ports, { color: C.red, width: 3.2, bidirectional: true });
  boxWithText(s, "L1 SWITCH\n0↔0  1↔1", { left: 579, top: 487, width: 122, height: 50 }, { fill: C.dark, lineFill: C.dark, color: C.paper, typeface: MONO, fontSize: 11, radius: 25 }).bringToFront();
  line(s, 310, 568, 0, 24, C.grayBlue, 1.5);
  line(s, 970, 568, 0, 24, C.grayBlue, 1.5);
  shape(s, "rect", { left: 60, top: 593, width: 1160, height: 70 }, C.dark, C.dark, 0);
  textBox(s, "macOS / Apple Silicon", { left: 82, top: 612, width: 270, height: 25 }, { typeface: MONO, fontSize: 16, bold: true, color: C.paper });
  textBox(s, "Docker Desktop ARM64 Linux · two gem5 24.0.0.1 processes", { left: 350, top: 612, width: 610, height: 25 }, { typeface: MONO, fontSize: 13, color: "#D0D5DE", alignment: "center" });
  textBox(s, "100 ns sync quantum", { left: 970, top: 612, width: 225, height: 25 }, { typeface: MONO, fontSize: 13, color: "#F0C3C6", alignment: "right" });
  addNotes(s, "当前 profile 是 fast/official：每节点一个 AtomicSimpleCPU，3 GHz，1 GB DDR3-1600；两个独立 gem5 进程分别启动 OLK 6.6 guest。OOB Ethernet 只负责资源交换，UB 数据走两条独立 400 Gbit/s 端口，经显式 L1 switch 映射。\n\n来源：" + path.join(LAB, "run-dual/run-manifest.txt"));
}

// 3. Official stack
{
  const s = deck.slides.add();
  header(s, "2 · 官方软件栈 / OFFICIAL STACK", "10 个官方内核模块从发现路径贯通到聚合数据面", 3);
  const vals = [
    ["层次", "官方模块", "当前已验证的职责"],
    ["URMA 核心", "ubcore.ko · uburma.ko", "设备、EID、Segment、Jetty与用户接口"],
    ["设备发现", "ubfi.ko · ubus.ko · hisi_ubus.ko", "UBIOS/UBC 表、资源窗口、配置消息与队列"],
    ["地址转换", "ummu-core.ko · ummu.ko", "TID、上下文绑定、页表遍历、队列与 payload DMA"],
    ["设备管理", "ubase.ko", "CmdQ、Mailbox、CtrlQ、事件与辅助设备"],
    ["数据面", "udma.ko", "udma0、JFC/JFR/JFS/Jetty、TP、SQE/CQE"],
    ["聚合", "ubagg.ko", "bonding_dev_0、双 primary plane 与双端口 balance"],
  ];
  const table = s.tables.add({ rows: vals.length, columns: 3, left: 60, top: 170, width: 1160, height: 350, columnWidths: [185, 390, 585], values: vals });
  table.borders.assign({ style: "solid", fill: "#000000", width: 1 });
  table.styleOptions = { headerRow: true, bandedRows: false };
  for (let r = 0; r < vals.length; r++) {
    table.rows[r].height = r === 0 ? 48 : 50;
    for (let c = 0; c < 3; c++) {
      const cell = table.getCell(r, c);
      cell.fill = r === 0 ? C.red : C.paper;
      cell.text.style = { typeface: c === 1 ? MONO : FONT, fontSize: r === 0 ? 16 : 16, bold: r === 0 || c === 0, color: r === 0 ? C.paper : C.ink, verticalAlignment: "middle", alignment: c === 0 ? "center" : "left", autoFit: "shrinkText" };
    }
  }
  metric(s, "10", "官方 UB 内核模块", { left: 60, top: 552, width: 250, height: 92 }, { valueSize: 35 });
  metric(s, "5 + 3", "官方库/provider + 官方工具", { left: 345, top: 552, width: 300, height: 92 }, { valueSize: 33 });
  metric(s, "0", "为仿真修改的官方驱动源码", { left: 680, top: 552, width: 300, height: 92 }, { valueSize: 35, valueColor: C.red, fill: C.redPale, lineFill: C.red });
  metric(s, "udma0", "当前官方数据设备", { left: 1015, top: 552, width: 205, height: 92 }, { valueSize: 28 });
  addNotes(s, "统计不包含 Linux 通用 ipv6.ko，也不包含我们自己的 openurma_ub_v2m.ko。10 个官方模块均以未修改源码加载；支持范围按已经通过的发现、控制和数据面路径计算，不等同每个模块的全部可选功能。\n\n来源：" + path.join(LAB, "overlay/init") + ":103-113；" + path.join(LAB, "official-udma/README.md") + ":20-45。")
}

// 4. Responsibility boundary
{
  const s = deck.slides.add();
  header(s, "3 · 代码边界 / RESPONSIBILITY", "软件继续做软件的事，模型只补硬件缺口", 4);
  textBox(s, "官方软件路径", { left: 60, top: 176, width: 360, height: 36 }, { fontSize: 25, bold: true, color: C.ink });
  const official = [
    ["应用", "urma_perftest / urma_admin / ubagg_cli"],
    ["UMDK", "liburma / UDMA provider / UBAGG provider"],
    ["OLK", "10 个官方 UB 内核模块"],
  ].map(([a,b], i) => boxWithText(s, `${a}\n${b}`, { left: 60, top: 230 + i * 96, width: 470, height: 72 }, { fill: C.paper, lineFill: C.border, fontSize: 17 }));
  connect(s, official[0], official[1], { kind: "straight", fromSide: "bottom", toSide: "top", color: C.grayBlue, width: 2 });
  connect(s, official[1], official[2], { kind: "straight", fromSide: "bottom", toSide: "top", color: C.grayBlue, width: 2 });
  textBox(s, "gem5 设备侧", { left: 735, top: 176, width: 360, height: 36 }, { fontSize: 25, bold: true, color: C.red });
  const sim = [
    ["发现与固件", "UBIOS / UBC / UBUS / UBASE 响应"],
    ["执行与 DMA", "UMMU 翻译、SQ/RQ/CQ、payload DMA"],
    ["网络与完成", "TP 路由、双端口传输、CQE 与 MSI"],
  ].map(([a,b], i) => boxWithText(s, `${a}\n${b}`, { left: 735, top: 230 + i * 96, width: 485, height: 72 }, { fill: C.redPale, lineFill: C.red, color: C.red, fontSize: 17, lineWidth: 1.6 }));
  connect(s, sim[0], sim[1], { kind: "straight", fromSide: "bottom", toSide: "top", color: C.red2, width: 2 });
  connect(s, sim[1], sim[2], { kind: "straight", fromSide: "bottom", toSide: "top", color: C.red2, width: 2 });
  connect(s, official[2], sim[0], { color: C.red, width: 3 });
  boxWithText(s, "MMIO · DMA · interrupt", { left: 554, top: 314, width: 158, height: 44 }, { fill: C.dark, lineFill: C.dark, color: C.paper, typeface: MONO, fontSize: 11, radius: 22 }).bringToFront();
  shape(s, "rect", { left: 60, top: 565, width: 1160, height: 73 }, C.dark, C.dark, 0);
  textBox(s, "不替换 WQE，不绕过 Doorbell，不在 benchmark 里复制远端内存", { left: 92, top: 585, width: 1096, height: 34 }, { fontSize: 22, bold: true, color: C.paper, alignment: "center", autoFit: "shrinkText" });
  addNotes(s, "官方 provider 仍创建对象、编码 WQE、写 Doorbell并轮询完成；官方内核驱动仍执行 probe、资源管理、Mailbox和TP控制。gem5 承担真实硬件应完成的队列消费、DMA、包处理和中断。唯一测试仪器化是可选的分布式虚拟时间边界。")
}

// 5. Hardware model
{
  const s = deck.slides.add();
  header(s, "4 · 仿真硬件 / DEVICE MODEL", "一个复合设备模型覆盖六组硬件契约", 5);
  const core = boxWithText(s, "NICTopologySC\n复合 UB endpoint", { left: 485, top: 283, width: 310, height: 116 }, { fill: C.dark, lineFill: C.dark, color: C.paper, typeface: MONO, fontSize: 22, radius: 58 });
  const blocks = [
    ["UBIOS / UBC", "设备发现与资源表", 60, 174],
    ["UBUS", "配置消息与资源窗口", 60, 430],
    ["UBASE", "CmdQ / Mailbox / CtrlQ", 865, 174],
    ["UMMU", "TID、页表与 IOTLB", 865, 430],
    ["UDMA", "WQE、队列、DMA、CQE", 315, 536],
    ["UB LINK", "双端口、L1 switch、时序", 705, 536],
  ].map(([a,b,x,y], i) => {
    const n = boxWithText(s, `${a}\n${b}`, { left: x, top: y, width: 355, height: 82 }, { fill: i >= 4 ? C.redPale : C.paper, lineFill: i >= 4 ? C.red : C.border, color: i >= 4 ? C.red : C.ink, typeface: i >= 4 ? MONO : FONT, fontSize: 17, lineWidth: i >= 4 ? 1.6 : 1 });
    connect(s, n, core, { fromSide: x < 400 ? "right" : (x > 800 ? "left" : "top"), toSide: x < 400 ? "left" : (x > 800 ? "right" : "bottom"), color: i >= 4 ? C.red : C.grayBlue, width: 1.8, arrow: false });
    return n;
  });
  core.bringToFront();
  labelText(s, "INHERITED PIPELINE", { left: 485, top: 174, width: 250, height: 20 }, C.grayBlue);
  textBox(s, "OpenURMA 原有 38 个 SystemC/TLM 模块作为 NIC 流水基础", { left: 430, top: 210, width: 420, height: 42 }, { fontSize: 17, color: C.body, alignment: "center", autoFit: "shrinkText" });
  addNotes(s, "按顶层源码对象统计，当前主路径是 NICTopologySC，单节点测试还使用 WireLoopback。按功能边界统计，复合模型覆盖六组硬件职责。原项目的 38 个 SystemC/TLM 模块属于继承基础，不应全部归为本轮从零编写。\n\n来源：" + path.join(OPENURMA, "eval/twonode/gem5_scaffold/src/NICTopologySC.hh") + ":1-20。")
}

// 6. Data path
{
  const s = deck.slides.add();
  header(s, "5 · 数据热路径 / RMA", "一条官方 READ WQE 如何跨两套 guest 完成", 6);
  const stages = [
    ["1", "官方 provider", "写 64 B WQEBB\n提交地址与长度"],
    ["2", "SQ Doorbell", "udma.ko 推进 PI\n模型捕获写入"],
    ["3", "UMMU + DMA", "按 TID 翻译 SQ\n读取 WQE 与 SGE"],
    ["4", "READ request", "TP 选择端口\n跨 peer ring"],
    ["5", "远端 DMA", "解析 rseg\n读取目标内存"],
    ["6", "response + CQE", "回写本地 SGE\n完整响应后完成"],
  ];
  const nodes = stages.map(([n,t,b], i) => {
    const x = 60 + (i % 3) * 395;
    const y = i < 3 ? 190 : 430;
    const box = boxWithText(s, `${n}  ${t}\n${b}`, { left: x, top: y, width: 340, height: 112 }, { fill: i >= 2 ? C.redPale : C.paper, lineFill: i >= 2 ? C.red : C.border, color: i >= 2 ? C.red : C.ink, fontSize: 18, lineWidth: i >= 2 ? 1.6 : 1 });
    return box;
  });
  connect(s, nodes[0], nodes[1], { color: C.grayBlue, width: 2 });
  connect(s, nodes[1], nodes[2], { color: C.red2, width: 2 });
  connect(s, nodes[2], nodes[3], { kind: "elbow", fromSide: "bottom", toSide: "top", color: C.red, width: 2 });
  connect(s, nodes[3], nodes[4], { color: C.red, width: 2 });
  connect(s, nodes[4], nodes[5], { color: C.red, width: 2 });
  textBox(s, "大 payload 仍只占一个 WQEBB。模型按 8088 B 最大载荷分片，全部响应到齐后只生成一个 CQE。", { left: 120, top: 595, width: 1040, height: 46 }, { fontSize: 20, bold: true, color: C.ink, alignment: "center", autoFit: "shrinkText" });
  addNotes(s, "READ 与 WRITE 均使用官方 provider 的固定格式 WQE。READ 的 payload 由远端 DMA 读取并分片返回；WRITE 则由发起端读取本地 SGE并在目标端写入。64 KiB READ/WRITE正式通过；单 WQE 的 1 MiB路径作为机制性诊断也已完成，但当前设备能力仍只声明64 KiB。\n\n来源：" + path.join(LAB, "official-udma/dual-node-perftest-evidence.md") + ":68-150。")
}

// 7. Aggregation topology
{
  const s = deck.slides.add();
  header(s, "6 · UB 聚合 / DUAL PLANE", "一个逻辑 EID 映射到两个 primary plane 和两个端口", 7);
  const logical = boxWithText(s, "bonding_dev_0\n逻辑聚合 EID 0x00200", { left: 90, top: 210, width: 270, height: 92 }, { fill: C.dark, lineFill: C.dark, color: C.paper, typeface: MONO, fontSize: 17, radius: 46 });
  const p0 = boxWithText(s, "plane 0\nprimary 0x00100\nport EID 0x20100", { left: 70, top: 380, width: 210, height: 104 }, { fill: C.redPale, lineFill: C.red, color: C.red, typeface: MONO, fontSize: 14, lineWidth: 1.7 });
  const p1 = boxWithText(s, "plane 1\nprimary 0x10100\nport EID 0x30100", { left: 315, top: 380, width: 210, height: 104 }, { fill: C.redPale, lineFill: C.red, color: C.red, typeface: MONO, fontSize: 14, lineWidth: 1.7 });
  connect(s, logical, p0, { kind: "elbow", fromSide: "bottom", toSide: "top", color: C.red, width: 2 });
  connect(s, logical, p1, { kind: "elbow", fromSide: "bottom", toSide: "top", color: C.red, width: 2 });
  textBox(s, "官方 balance provider 为两个 primary EID 分别建立 context、Jetty 与 TP。两个 plane 都是活动路径，不是主备关系。", { left: 65, top: 525, width: 480, height: 78 }, { fontSize: 18, color: C.ink, alignment: "center", autoFit: "shrinkText" });
  addImage(s, resourceBytes, "image/png", { left: 585, top: 175, width: 625, height: 352 }, "official resource and dual-port evidence", "contain");
  textBox(s, "真实日志：两个 Jetty 使用独立 doorbell，TP 5 绑定 port 0，TP 6 绑定 port 1", { left: 600, top: 547, width: 595, height: 48 }, { fontSize: 18, bold: true, color: C.ink, alignment: "center", autoFit: "shrinkText" });
  textBox(s, "urma_admin 的 5 个可见 EID = 1 个逻辑聚合身份 + 2 个 primary EID + 2 个 port EID", { left: 90, top: 625, width: 1100, height: 30 }, { typeface: MONO, fontSize: 15, color: C.red, alignment: "center", autoFit: "shrinkText" });
  addNotes(s, "官方 ABI 名为 io_die_info[2]，但 UMDK balance 路径明确按两个 data-plane plane 使用。standalone 只使用 plane 0，并不会在同一记录的两个 port EID 间自动 balance。\n\n来源：" + path.join(LAB, "official-udma/ubagg-topology-evidence.md") + ":12-45；截图：" + IMG_RESOURCES);
}

// 8. Virtual time
{
  const s = deck.slides.add();
  header(s, "7 · 时间与链路 / VIRTUAL TIME", "跨进程推进遵循 100 ns 保守同步边界", 8);
  shape(s, "rect", { left: 60, top: 166, width: 760, height: 58 }, C.dark, C.dark, 0);
  textBox(s, "t_arrival = t_send + serialization(payload, 400G) + 100 ns", { left: 78, top: 181, width: 724, height: 28 }, { typeface: MONO, fontSize: 19, bold: true, color: C.paper, alignment: "center", autoFit: "shrinkText" });
  const a = boxWithText(s, "node 0\n发布带 arrival tick 的 packet", { left: 90, top: 310, width: 250, height: 92 }, { fill: C.paper, lineFill: C.border, fontSize: 17 });
  const b = boxWithText(s, "100 ns\nlookahead window", { left: 500, top: 310, width: 230, height: 92 }, { fill: C.redPale, lineFill: C.red, color: C.red, typeface: MONO, fontSize: 16, lineWidth: 2 });
  const c = boxWithText(s, "node 1\n到达安全边界后消费 packet", { left: 880, top: 310, width: 250, height: 92 }, { fill: C.paper, lineFill: C.border, fontSize: 17 });
  connect(s, a, b, { color: C.red2, width: 2.5 });
  connect(s, b, c, { color: C.red2, width: 2.5 });
  line(s, 130, 485, 960, 0, C.grayBlue, 1.5);
  for (let i = 0; i < 6; i++) {
    const x = 130 + i * 192;
    line(s, x, 476, 0, 18, i === 0 || i === 5 ? C.red : C.border, i === 0 || i === 5 ? 2 : 1);
    textBox(s, `${i * 100} ns`, { left: x - 35, top: 510, width: 80, height: 18 }, { typeface: MONO, fontSize: 10, color: C.grayBlue, alignment: "center" });
  }
  textBox(s, "宿主调度只影响仿真跑得快慢，不改变 packet 的虚拟到达顺序", { left: 185, top: 555, width: 910, height: 40 }, { fontSize: 22, bold: true, color: C.ink, alignment: "center", autoFit: "shrinkText" });
  textBox(s, "当前未增加拟合实测的固定 DMA / WQE / switch service delay", { left: 250, top: 610, width: 780, height: 28 }, { typeface: MONO, fontSize: 14, color: C.red, alignment: "center" });
  addNotes(s, "链路按每端口独立 400 Gbit/s 序列化时间线推进，一程传播100 ns；两个gem5进程每100 ns建立保守同步边界。当前 fixed service add-ons 与 switch delay均为0，结果用于验证因果关系与趋势，不用额外延迟项拟合真实交换机绝对值。\n\n来源：" + path.join(LAB, "run-dual/run-manifest.txt"));
}

// 9. Experiment matrix
{
  const s = deck.slides.add();
  header(s, "8 · 已验证实验 / COVERAGE", "官方路径已完成 SEND、READ、WRITE 与双端口聚合", 9);
  const vals = [
    ["路径", "规模", "结果", "证据含义"],
    ["CTP SEND_IMM", "128 B", "双端返回 0", "官方 SQE、RQE、CQE 与跨节点往返"],
    ["WRITE bandwidth", "128 B", "349.10 MiB/s", "小包 WQE 与目标端 DMA"],
    ["WRITE bandwidth", "8 KiB", "22,217.84 MiB/s", "8088 + 104 B 分片与 ACK"],
    ["WRITE bandwidth", "64 KiB", "44,074.69 MiB/s", "9 个 fragment，单个 CQE"],
    ["READ bandwidth", "128 B", "731.43 MiB/s", "请求、远端 DMA、响应、本地 DMA"],
    ["READ bandwidth", "8 KiB", "22,923.99 MiB/s", "双端同时发流，完整返回"],
    ["READ bandwidth", "64 KiB", "24,466.37 MiB/s", "正式 capability 上限内通过"],
    ["UBAGG balance", "128 B READ", "2.88 µs", "两个 primary plane 在 port 0/1 交替"],
  ];
  const table = s.tables.add({ rows: vals.length, columns: 4, left: 60, top: 165, width: 1160, height: 430, columnWidths: [250, 170, 210, 530], values: vals });
  table.borders.assign({ style: "solid", fill: "#000000", width: 1 });
  table.styleOptions = { headerRow: true, bandedRows: false };
  for (let r = 0; r < vals.length; r++) {
    table.rows[r].height = r === 0 ? 46 : 48;
    for (let c = 0; c < 4; c++) {
      const cell = table.getCell(r, c);
      cell.fill = r === 0 ? C.red : (r === vals.length - 1 ? C.redPale : C.paper);
      cell.text.style = { typeface: c <= 2 ? MONO : FONT, fontSize: r === 0 ? 15 : 14.5, bold: r === 0 || c === 0, color: r === 0 ? C.paper : (r === vals.length - 1 ? C.red : C.ink), verticalAlignment: "middle", alignment: c === 3 ? "left" : "center", autoFit: "shrinkText" };
    }
  }
  textBox(s, "正式 READ/WRITE capability 当前为 64 KiB；1 MiB 单 WQE 仅作为机制性诊断，不写进正式设备能力。", { left: 80, top: 620, width: 1120, height: 32 }, { fontSize: 17, bold: true, color: C.ink, alignment: "center", autoFit: "shrinkText" });
  addNotes(s, "带宽数字来自五次迭代的结构性验证，不用于与真实板卡作性能对标。实验说明 WQE、UMMU、fragment streaming、远端DMA和CQE链路已经闭合。UBAGG READ latency只有两条测量样本，作用是功能证据。\n\n来源：" + path.join(LAB, "official-udma/dual-node-perftest-evidence.md") + ":68-150；" + path.join(LAB, "official-udma/ubagg-dataplane-evidence.md") + ":118-190。")
}

// 10. Command evidence
{
  const s = deck.slides.add();
  header(s, "9.1 · 运行证据 / COMMAND", "官方 urma_perftest 完成 UBAGG READ 时延测试", 10);
  addImage(s, readResultBytes, "image/png", { left: 60, top: 156, width: 1160, height: 522 }, "official urma_perftest READ latency evidence", "contain");
  addNotes(s, "截图由真实 PL011 UART 记录生成，仅合并终端自动换行，数值未改写。命令使用 bonding_dev_0、CTP、balance和128 B READ；两次连续运行都返回0。当前短样本只用于证明可重复执行。\n\n截图：" + IMG_READ_RESULT)
}

// 11. Packet evidence
{
  const s = deck.slides.add();
  header(s, "9.2 · 包级证据 / PACKETS", "READ 请求与响应在两个端口间交替", 11);
  addImage(s, portPacketBytes, "image/png", { left: 60, top: 156, width: 1160, height: 522 }, "dual-port READ request and response evidence", "contain");
  addNotes(s, "日志显示 seq 16/18 使用 port 1 和TP 6，seq 17/19 使用 port 0 和TP 5；响应保持原入口端口返回。该证据说明分流来自官方 provider创建的两个物理Jetty/TP与模型执行的端口路由，而不是benchmark硬编码。\n\n截图：" + IMG_PORT_PACKETS)
}

// 12. Scope and close
{
  const s = deck.slides.add();
  s.background.fill = C.paper;
  shape(s, "rect", { left: 0, top: 0, width: 12, height: 720 }, C.red, C.red, 0);
  textBox(s, "当前边界", { left: 72, top: 62, width: 380, height: 58 }, { fontSize: 40, bold: true, color: C.ink });
  line(s, 74, 136, 110, 0, C.red, 4);
  textBox(s, "已经打通", { left: 72, top: 178, width: 380, height: 36 }, { fontSize: 24, bold: true, color: C.red });
  const done = [
    "10 个官方 UB 内核模块",
    "官方 UDMA 与 UBAGG provider",
    "双节点 SEND / READ / WRITE",
    "双 primary plane 与双物理端口",
    "UMMU 支持的队列与 payload DMA",
  ];
  done.forEach((t, i) => {
    textBox(s, String(i + 1).padStart(2, "0"), { left: 72, top: 232 + i * 58, width: 38, height: 22 }, { typeface: MONO, fontSize: 13, bold: true, color: C.red });
    textBox(s, t, { left: 120, top: 226 + i * 58, width: 440, height: 34 }, { fontSize: 18, color: C.ink, autoFit: "shrinkText" });
  });
  textBox(s, "仍需补齐", { left: 680, top: 178, width: 380, height: 36 }, { fontSize: 24, bold: true, color: C.grayBlue });
  const todo = [
    "CDMA、OBMM、Sentry 等可选硬件",
    "官方 bonding-group 表与单 TP 多端口散列",
    "UMMU cfg_table 销毁告警",
    "更完整的异常、恢复与压力覆盖",
    "面向具体硬件配置的绝对性能标定",
  ];
  todo.forEach((t, i) => {
    textBox(s, String(i + 1).padStart(2, "0"), { left: 680, top: 232 + i * 58, width: 38, height: 22 }, { typeface: MONO, fontSize: 13, bold: true, color: C.grayBlue });
    textBox(s, t, { left: 728, top: 226 + i * 58, width: 470, height: 34 }, { fontSize: 18, color: C.ink, autoFit: "shrinkText" });
  });
  shape(s, "rect", { left: 72, top: 560, width: 1126, height: 72 }, C.dark, C.dark, 0);
  textBox(s, "当前成果是一条可运行、可观察、可继续扩展的官方 UB 全栈仿真主链路", { left: 105, top: 580, width: 1060, height: 34 }, { fontSize: 23, bold: true, color: C.paper, alignment: "center", autoFit: "shrinkText" });
  textBox(s, "Questions & Discussion", { left: 72, top: 660, width: 400, height: 25 }, { typeface: MONO, fontSize: 15, color: C.grayBlue });
  addNotes(s, "收尾明确区分已验证范围与下一步。当前可以称为官方主链路已经贯通，但不能称为所有UB硬件和全部驱动功能均已实现。")
}

const requirements = {
  explicitTotalSlideCount: 12,
  requiredNativeTableOwnerSlides: [3, 9],
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
    "--require-native-table-slide", "3",
    "--require-native-table-slide", "9",
  ],
  requiredNativeTableOwnerSlides: [3, 9],
  requiredNativeChartOwnerSlides: [],
  fontPolicy,
  verifyArtifactToolImport: true,
  receiptPath: path.join(stagingDir, `${path.basename(FINAL_PPTX)}.validation.json`),
});

console.log(JSON.stringify({ finalPath: FINAL_PPTX, result }, null, 2));

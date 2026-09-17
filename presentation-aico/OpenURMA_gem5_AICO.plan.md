# OpenURMA × gem5 双节点全系统仿真 · 页面规划

## 场景与口径

- 场景：10 分钟左右的系统技术分享，现场讲解并交付 PPTX。
- 听众：熟悉系统仿真、驱动或高性能互连的研发人员。
- 目标：先让听众看到双节点如何运行，再讲官方软件跑到哪里、仿真层补了什么、一次 SEND_IMM 如何走完，以及怎样理解当前结果。
- 视觉：AICO-PPT 技术分享外壳，深色技术封面，内容页使用白底、灰结构和红色数据路径；不沿用企业 Logo、密级与免责声明。

## 大纲

- 01 系统全景：宿主、两套 guest 与 UB 数据路径（2 页）
- 02 代码边界：官方 provider 与 gem5 设备模型如何分工（2 页）
- 03 执行机制：一次 SEND_IMM 如何跨节点完成（2 页）
- 04 运行与观察：怎样复现，以及结果如何解释（2 页）

## 逐页

| # | label | 这页要说啥 | AICO 页型参考 | 配图 / 证据 |
|---:|---|---|---|---|
| 1 | 封面 | 把 OpenURMA 跑进 gem5：双节点全系统仿真、设备模型与请求路径 | tech-share `cover` | 深灰技术纹理 + 双节点链路 |
| 2 | 目录 | 按先看效果、再拆路径、最后动手复现的顺序展开 | tech-share `toc` | 目录关系图 |
| 3 | 目标实现 | macOS / Apple Silicon 宿主上的两个 gem5 进程，各自运行完整 ARM64 guest、UB 软件栈与测试进程 | tech-share `architecture` | 可编辑分层运行栈框图 |
| 4 | 运行现象 | 两套 Linux 可独立登录，设备 ACTIVE，跨节点 SEND_IMM 完成 | tech-share `agenda` | 三个观察量 + 请求路径 |
| 5 | 代码边界 | 官方原样、官方工具适配与仿真新增如何分工 | tech-share `architecture` | 可编辑标准表格 |
| 6 | 数据热路径 | 官方 provider 写 WQE 与 doorbell，gem5 完成 DMA、peer 与 CQE | tech-share `code-curve-metrics` | 官方源码 + 仿真源码 + 可编辑路径图 |
| 7 | 虚拟时间 | 100 ns 同步量子约束双进程因果推进，链路只建模可解释机制 | tech-share `mechanism` | 可编辑时序图 |
| 8 | 运行观察 | 日志显示设备已注册，`urma_admin` 显示本地端口 ACTIVE | tech-share `screenshot` | 用户截图 1、2 |
| 9 | 动手复现 | 两端各一条命令启动同一 CTP/RM/SEND_IMM 测试 | tech-share `live-demo` | 原生命令框 + 运行关系图 |
| 10 | 结果解读 | 解释 20 个样本下 p99 为什么等于最大值，并给出后续扫描方式 | tech-share `metrics` + `takeaway` | 用户截图 3 + 大数字 |
| 11 | 结语页 | Thank you / Q&A | tech-share `thanks` | 纯白收尾 |

## 事实与来源

- 当前运行参数：`run-dual/run-manifest.txt`，ArmAtomicSimpleCPU 3 GHz、1 核、无 CPU cache、1 GB DDR3-1600、400 Gbit/s、100 ns propagation、100 ns sync quantum、`pipe_data=0`，固定 WQE/SQ fetch/payload DMA 附加延迟为 0。
- 当前宿主：macOS / Apple Silicon；本机核实为 MacBook Air、Apple M4、16 GB。Docker Desktop 中的 aarch64 Ubuntu 22.04 容器运行两个 gem5 24.0.0.1 进程。
- guest 口径：openEuler OLK 6.6 ARM64 内核 + BusyBox initramfs，不写成完整 openEuler 用户态发行版。
- 官方且实际运行：OLK 6.6 `ubcore.ko`、`uburma.ko`，`urma_admin`，官方 UDMA userspace provider `liburma-udma.so`。
- 官方基础上适配：`urma_perftest` 的 gem5 时间源、双端虚拟时间同步、ROI/CPU 切换、预热排除和 nearest-rank 百分位。
- 本工程新增：`openurma_ubcore.ko`、控制 ABI、`NICTopologySC` UDMA 行为、peer ring、双节点同步和启动/接入脚本、仿真用 `libummu.so.1` shim。
- 官方 kernel UDMA 硬件驱动未启用：`build_olk66.sh:70-85` 中 `CONFIG_UB_UDMA` 关闭。
- 官方 provider 关键路径：`integration/umdk/vendor/umdk/src/urma/hw/udma/udma_u_jfs.c:821-837` 与 `udma_u_jfs.h:129-133`。
- 仿真 NIC 关键路径：`NICTopologySC.cc:2402-2421`、`:1440-1457`、`:1504-1523`、`:1278-1287`、`:2239-2250`、`:2147-2187`。
- 当前范围：normal pinned memory、duplex Jetty、CTP/RM SEND/SEND_IMM。非 pin、专用大页、simplex JFS、严格异步 drain 尚未覆盖。
- 20 个样本采用 nearest-rank 百分位时，`ceil(0.99 × 20) = 20`，因此 p99 取最大样本。

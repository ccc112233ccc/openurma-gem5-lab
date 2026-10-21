# 工作区交接说明

本文件是继续本仓库工作时的入口。先阅读[文档目录](README.md)，再按任务选择[架构](architecture.md)、[同步](synchronization.md)、[官方软件栈集成](guest-driver-integration.md)、[编译与运行](build-and-run.md)、[拓扑编排](topology-orchestration.md)。以当前源码、解析后的运行 manifest 和具体镜像/checkpoint 指纹为准，不要直接沿用某次实验的“当前值”。

## 当前可用路径

- gem5 全系统：ARM64 官方 OLK/UMDK 客体、独立 UDMA、ns-3-UB；`modern` 声明式任务通过快速启动与 ROI 阶段的 ArmO3/跨进程同步运行。固定配置优先恢复兼容 checkpoint。
- QEMU TCG：共享同一 UDMA/网络模型；`execution.sync=off` 是功能模式，`required` 启用 `icount` 和进程间同步。两者结果不可混作同一条性能曲线。
- JSON 拓扑：gem5 和 QEMU 各支持 2–8 节点的单交换机布局，两节点还可直连。QEMU 可异构，gem5 节点使用同一机器 profile。详细限制见[拓扑编排](topology-orchestration.md)。
- 4 GB `modern` gem5 客体有节点模板 checkpoint，可复用同一基础状态建立多个独立节点；其他镜像或机器参数需逐项检查兼容性。

## 工作习惯

日常功能开发用 `./lab start-ready` 恢复兼容快照；仅在明确需要新内核启动、驱动初始化或新 checkpoint 时冷启动。功能检查用非同步模式，计时结论用有共同虚拟时间轴的模式并保存完整输入和原始输出。启动前后要检查宿主机内存保护和运行目录的状态日志。

早期逐日实验、性能定位与修复过程原文保存在[历史交接材料](validation/handoff-history-20261021.md)。它包含不同日期、不同镜像和配置的结果，只用于追溯证据；不要把其中某一轮数字直接当作当前工程的性能。需要核对实测对照时，从[验证资料索引](validation/README.md)找到对应实验，再检查 `experiments/` 和 `results/` 的原始文件。

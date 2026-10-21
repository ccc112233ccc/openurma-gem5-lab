# 编译与运行

## 运行前确认

仓库根目录的 `./lab` 是宿主机入口，声明式任务由 `./lab topology ...` 读取 JSON 并直接启动进程。`scripts/` 仍是构建和旧命令行路径的内部后端。运行前先执行 `./lab --help`，并核对所选 JSON 的 `execution.sync`、`execution.checkpoint`、内存容量与主机类型。

完整 gem5 全系统路径需要 gem5 可执行文件、ARM64 内核与 initramfs、独立 UDMA、ns-3-UB，以及匹配的 checkpoint；QEMU 路径另用 QEMU 可执行文件和其客体镜像。当前 Apple Silicon 工作区保留了这些产物，但新克隆不会天然具备。Linux 可以重建官方客体软件；macOS 原生构建 gem5/QEMU 与模型进程，不能在本机直接重建 ARM64 Linux 客体内核和 UMDK。

## 使用已有产物运行

建议先校验和查看启动计划，再启动。以下命令使用两节点、单交换机、同步 gem5 配置：

```sh
./lab topology validate configs/topology-gem5-two-heads.json
./lab topology plan configs/topology-gem5-two-heads.json
./lab topology start configs/topology-gem5-two-heads.json
./lab status
./lab sync
./lab rma-regression --sync-smoke
./lab stop
```

`topology start` 的 `checkpoint=auto` 优先选择指纹兼容的节点模板或协调 checkpoint；没有匹配项时会先检查宿主机内存，再冷启动。日常固定拓扑实验可用 `./lab start-ready ...` 恢复最近的兼容 checkpoint。不要把 `./lab start ...` 当作常规回归入口；它适用于明确需要重新启动内核、初始化客体或创建新 checkpoint 的工作。checkpoint 的复用范围见[节点模板设计](node-template-checkpoint.md)。

QEMU 功能检查示例：

```sh
./lab topology validate configs/topology-qemu-two-heads.json
./lab topology plan configs/topology-qemu-two-heads.json
./lab topology start configs/topology-qemu-two-heads.json
./lab status-qemu
./lab qemu-rma-regression
./lab stop-qemu
```

若需要 QEMU 的共同虚拟时间轴，把任务的 `execution.sync` 设为 `required`；`off` 只用于功能检查。直连示例分别为 `configs/topology-gem5-direct-link.json` 和 `configs/topology-qemu-direct-link.json`。多节点、异构机头和链路限制见[拓扑编排](topology-orchestration.md)。

`./lab attach 0` 或 `./lab attach-qemu 0` 接入指定节点串口；行首 `~.` 断开。`./lab ui` 打开本地工作台，界面流程见[工作台说明](ui-workbench.md)。

## 首次构建和增量构建

在支持的 ARM64 Linux 宿主机上，`./lab setup --jobs 2` 获取固定版本源码并完成依赖与主体构建；也可按模块执行 `./lab build TARGET`。Apple Silicon 上用原生构建脚本生成 gem5/QEMU 与设备、网络进程。支持的 TARGET 列表见 `./lab --help`，具体前提以对应构建脚本和日志为准。

| 修改范围 | 常用命令 | 关键产物 |
| --- | --- | --- |
| gem5 平台或 UB-HOST | `./lab build gem5` | `gem5/build/ARM/gem5.opt` 或 `ARM_MAC/gem5.opt` |
| QEMU 平台或 UB-HOST | `./lab build qemu` | QEMU 可执行文件 |
| UDMA 设备模型 | `./lab build udma-model`，`./lab build udma-device` | 设备模型与独立进程 |
| UB 网络 | `./lab build ns3ub` | ns-3-UB 适配器 |
| 官方 UMDK/provider | Linux：`./lab build umdk` | ARM64 用户态库与工具 |
| 官方 OLK/模块 | Linux：`./lab build kernel` | `artifacts/kernel/` |
| 客体 initramfs | Linux：`./lab build initramfs` | `out/official-udma.cpio.gz` |

官方软件栈的构建和加载顺序见[官方软件栈集成](guest-driver-integration.md)。`./lab build all` 目前**不会**重建工作区保留的 64 KiB 客体镜像：`out/official-udma-64k-20261015.cpio.gz` 与 `out/official-udma-64k-node-template-20261021.cpio.gz`。改变镜像内容会改变 SHA-256，原 checkpoint 可能失配。不要用旧 checkpoint 强行配新内核或 initramfs。

## 产物与运行保护

- `configs/topology-*.json`：任务输入；`ubsim_lab/topology.py` 校验图结构；`ubsim_lab/config_runtime.py` 解析机器与同步参数。
- `checkpoints/`：gem5 快照；`run-dual/` 和 `run-qemu-dual/`：当前运行状态、解析后的 manifest、进程日志和串口输出。
- `experiments/` 与 `results/`：实验原始产物和整理结果；比较性能时应连同拓扑、镜像来源和工作负载命令保存。

启动前及运行中有宿主机内存保护；触发时在运行目录查看 `memory-guard.log` 和 `memory-guard.stopped`。多节点启动前应使用 `topology plan` 看清每台客体的建模内存和所需进程，避免在小内存宿主机上同时启动多个大容量 gem5 客体。独立运行目录可通过 `UBSIM_DUAL_OUT` 指定，便于保留实验之间的 manifest 与日志。

新配置没有可用 checkpoint 时，只冷启动一次并保存对应快照：固定节点状态使用 `./lab checkpoint NAME`，可变节点数的前身份模板使用 `./lab checkpoint-template NAME`。创建流程与兼容条件见[节点模板 checkpoint](node-template-checkpoint.md)。

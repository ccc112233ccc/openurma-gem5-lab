# UBSim：gem5/QEMU + 官方 URMA + UB 网络仿真实验平台

本工程在 ARM64 全系统客体中运行 openEuler OLK UB 驱动、UMDK 和 `urma_perftest`。机头可选择 gem5 或 QEMU；每台机头连接一份独立 UDMA 模型，设备再接入一个 ns-3-UB 网络进程。gem5 用于 CPU、缓存、内存与 UB 路径的细粒度实验，QEMU 用于较快的功能验证及受同步约束的模型实验。

**先看[中文文档目录](docs/README.md)**。其中有[系统架构](docs/architecture.md)、[同步策略](docs/synchronization.md)、[设备与网络边界](docs/ns3ub-network-boundary.md)、[官方驱动集成](docs/guest-driver-integration.md)、[编译运行](docs/build-and-run.md)及[拓扑编排](docs/topology-orchestration.md)。以这些章节和当前源码为准；过往实验保留在 `docs/validation/`、`results/`、`experiments/`。

## 使用已有工作区

在仓库根目录运行两节点 gem5 交换拓扑：

```sh
./lab topology validate configs/topology-gem5-two-heads.json
./lab topology plan configs/topology-gem5-two-heads.json
./lab topology start configs/topology-gem5-two-heads.json
./lab status
./lab sync
./lab rma-regression --sync-smoke
./lab stop
```

功能性 QEMU 示例：

```sh
./lab topology validate configs/topology-qemu-two-heads.json
./lab topology start configs/topology-qemu-two-heads.json
./lab status-qemu
./lab qemu-rma-regression
./lab stop-qemu
```

配置中的 `execution.sync` 决定是否使用共同虚拟时间轴。功能模式的客体带宽/延迟输出不能直接当作仿真计时结果。`checkpoint=auto` 会选择兼容的 gem5 checkpoint，找不到时按内存保护规则决定是否冷启动。当前工作区保留部分镜像和 checkpoint；新克隆需要按[编译与运行](docs/build-and-run.md)准备产物。所有宿主机命令均以 `./lab` 为入口，具体参数可查看 `./lab --help`。

## 工作台与目录

`./lab ui` 启动本地工作台，可创建任务、查看生成的拓扑、设置主机和链路参数、启动仿真、连接对应客体终端并运行双机实验。使用细节见[工作台说明](docs/ui-workbench.md)。

| 目录 | 用途 |
| --- | --- |
| `ubsim_lab/`、`lab` | 配置校验、启动计划、任务与工作台 |
| `configs/` | gem5 全系统平台和拓扑 JSON 示例 |
| `protocol/` | `UB-HOST`、`UB-NET` 进程边界 |
| `components/` | 独立 UDMA 设备模型及契约参考组件 |
| `integrations/` | gem5、QEMU 与 ns-3-UB 的薄适配层 |
| `official-udma/`、`overlay/` | 官方客体软件栈构建与镜像启动文件 |
| `scripts/`、`tools/` | 构建、运行、验证辅助程序 |
| `patches/`、`SOURCE_REVISIONS.md` | 固定上游版本与可审查的变更 |
| `docs/validation/`、`results/`、`experiments/` | 历史实验、汇总结果和原始运行产物 |

旧版首页的完整文本已归档在[历史首页](docs/validation/repository-readme-history-20261021.md)，只供追溯，不代表当前接口和参数。

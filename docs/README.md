# 文档目录

本目录区分**当前实现说明**与**历史验证材料**。设计、运行和接口以本页所列的正式文档及当前源码为准；`validation/`、`results/`、`experiments/` 中的记录保留了当时的配置和观测值，不能直接当作现行默认配置或性能承诺。

## 从哪里开始

| 目的 | 文档 |
| --- | --- |
| 了解系统由哪些进程组成、一次 READ/WRITE 怎样走完 | [系统架构](architecture.md) |
| 理解时间同步、延迟归属及何时能解释时延数据 | [仿真同步与计时](synchronization.md) |
| 理解 UDMA、UB-NET、原生 ns-3-UB 的职责 | [设备与网络模型](ns3ub-network-boundary.md) |
| 查看 UDMA 的控制面、数据面与模型容量 | [UDMA 设备模型](udma-device.md) |
| 了解官方 OLK/UMDK 如何进入客体，与 gem5 的关系 | [官方软件栈集成](guest-driver-integration.md) |
| 编译、启动、恢复 checkpoint、查看日志 | [编译与运行](build-and-run.md) |
| 编排交换或直连拓扑，理解 JSON 字段和限制 | [拓扑编排](topology-orchestration.md) |
| 复用一份节点模板 checkpoint | [节点模板 checkpoint](node-template-checkpoint.md) |
| 使用可视化工作台 | [工作台使用说明](ui-workbench.md) |

## 源码与证据

- [源码归属与版本边界](ubsim-source-boundary.md)说明上游源码、项目自有模型与集成代码的位置；准确版本见仓库根目录的 [`SOURCE_REVISIONS.md`](../SOURCE_REVISIONS.md)。
- [验证记录索引](validation/README.md)标明哪些材料是实验记录。原始结果不自动代表当前代码与镜像的表现。
- `docs/NEXT_SESSION.md` 是当前工作区的交接入口；早期逐日过程归档在 `validation/handoff-history-20261021.md`。遇到结果不一致时，应先核对对应实验的 manifest、源码版本和当前启动配置。

本文档只描述已实现的路径。性能校准结论须同时给出客体镜像、checkpoint、主机模型、网络参数、同步模式、负载命令和原始输出。

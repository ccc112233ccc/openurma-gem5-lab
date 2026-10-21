# 系统架构

## 范围与目标

本项目在 ARM64 全系统客体中运行官方 openEuler UB 内核栈、UMDK 和 `urma_perftest`，使用独立进程模拟 UDMA 设备与 UB 网络。gem5 用于需要 CPU、缓存、内存和设备交互计时的实验；QEMU TCG 共享相同设备与网络模型，主要用于功能验证，也提供受同步约束的计时模式。仿真结果是否接近实机，应针对给定工作负载和配置验证，不能由组件连通性推出。

## 进程和接口

```text
客体 OLK/UMDK/urma_perftest
          │
gem5 或 QEMU（CPU、内存、平台中断）
          │ UB-HOST：MMIO、DMA、MSI/中断、时间
独立 UDMA 设备进程（每台主机一份）
          │ UB-NET：完整 WQE 封套、链路状态、时间
ns-3-UB 网络进程（一个交换结构或一条直连链路）
```

N 台主机对应 N 个机头进程、N 个 UDMA 进程和 1 个 ns-3-UB 进程，即 `2N+1` 个主体进程。客体另有独立的带外以太网连接，用于 `urma_perftest` 两端交换连接参数；业务数据由 EID/TP 在 UB 数据路径中转发。

`UB-HOST` 是机头与设备之间的接口。gem5 的 [`UbHostAdapter`](../integrations/gem5/ub_host_adapter/) 和 QEMU 的[适配器](../integrations/qemu/)只桥接地址空间、DMA、设备中断和时钟，不实现 WQE、Jetty 或 CTP。设备核心在 [`components/udma-model`](../components/udma-model/)；[`udma-device-sim`](../components/udma-device-sim/)把该模型作为独立进程接入两个接口。

## 操作路径

1. 客体 provider 通过官方驱动建立 JFC/JFS/JFR/Jetty、TP、内存段与令牌，写入 SQE 并敲门铃。
2. UDMA 模型解析官方队列格式，经 `UB-HOST` 请求客体内存 DMA，校验令牌与地址，选择物理端口，然后把完整操作封套交给 `UB-NET`。
3. ns-3-UB 端点的原生 CTP/Jetty 状态机负责传输层分段、顺序、TASSN、窗口与可选重传；交换或直连模型负责链路、转发、序列化和排队。
4. 目标 UDMA 对收到的请求执行客体内存 DMA；完成或响应沿反向路径返回。发起端 UDMA 按操作结果写 CQE，并通过客体中断路径通知驱动/provider。

大 WQE 封套在共享内存 IPC 环上可能被拆成若干记录；这只是**进程间传输分片**。READ/WRITE 在 CTP 中的分段由原生 ns-3-UB 端点完成，当前最大段载荷为 4 KiB。小于 4 KiB 的有效数据不会因此填充到 4 KiB。

## 软件和模型的归属

| 层 | 主要源码 | 负责内容 |
| --- | --- | --- |
| 客体软件 | `oe66/`、`deps/ummu/`、`sources/umdk/` | OLK 驱动、UMMU、liburma、官方 provider 与工具 |
| 机头平台 | `configs/arm64_fs.py`、`integrations/gem5/`、`integrations/qemu/` | CPU、内存、平台和 UB-HOST 适配 |
| 设备硬件模型 | `components/udma-model/`、`components/udma-device-sim/` | 发现/寄存器、固件控制队列、DMA、资源、数据队列、CQE、中断 |
| 网络硬件模型 | `integrations/ns3ub/`、`sources/ns-3-ub/` | 原生 CTP 端点、物理端口、交换/直连、队列与链路 |
| 编排与结果 | `ubsim_lab/`、`lab`、`configs/topology-*.json` | 配置校验、启动计划、运行状态与任务产物 |

当前支持的拓扑范围与 checkpoint 规则见[拓扑编排](topology-orchestration.md)；时间边界见[仿真同步与计时](synchronization.md)。模拟器的行为不等同于公开 URMA 规范的完整实现；具体覆盖范围与模型假设见[官方驱动覆盖审计](validation/official-driver-coverage.md)。

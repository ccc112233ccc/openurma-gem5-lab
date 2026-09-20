# OpenURMA × gem5 官方全栈仿真技术分享

## 场景与口径

- 场景：面向系统仿真、驱动和高性能互连研发人员的技术分享。
- 主线：保留官方驱动与 provider 的真实职责，由 gem5 补齐设备发现、控制面、地址转换、队列执行和双端口传输。
- 视觉：沿用现有 AICO 技术分享风格，深色封面、白底正文、红色数据路径。
- 事实基线：`run-dual/run-manifest.txt`、`official-udma/*.md`、`evidence/official-rma-2026-09-19/`。

## 逐页结构

| # | 页面 | 核心内容 |
|---:|---|---|
| 1 | 封面 | 官方 UB 全栈进入 gem5，覆盖设备发现、UMMU/UDMA、双端口聚合和 RMA |
| 2 | 系统全景 | macOS/Apple Silicon、两个 gem5、OLK 6.6、官方 UMDK、AtomicSimpleCPU、双 400G 端口 |
| 3 | 官方软件栈 | 10 个官方 UB 内核模块及其已验证职责 |
| 4 | 代码边界 | 官方软件与 gem5 设备侧职责分工 |
| 5 | 仿真硬件 | NICTopologySC 覆盖六组硬件契约，继承 38 个 SystemC/TLM 模块 |
| 6 | 数据热路径 | 官方 READ WQE、UMMU、远端 DMA、响应和 CQE |
| 7 | UB 聚合 | 逻辑聚合 EID、两个 primary plane、两个 port EID |
| 8 | 虚拟时间 | 400G 序列化、100 ns 传播和 100 ns 保守同步 |
| 9 | 实验覆盖 | SEND、READ、WRITE、UBAGG balance与正式64 KiB能力范围 |
| 10 | 命令证据 | 官方 `urma_perftest read_lat` 连续运行成功 |
| 11 | 包级证据 | READ request/response 在 port 0/1 间交替并原端口返回 |
| 12 | 当前边界 | 已打通主链路和待补齐的可选硬件、异常路径与性能标定 |

## 关键事实

- 官方路径加载 `ubcore`、`uburma`、`ubfi`、`ubus`、`hisi_ubus`、`ummu-core`、`ummu`、`ubase`、`udma`、`ubagg` 共 10 个模块。
- 官方 UDMA、UBAGG provider 和 OLK 驱动源码未因仿真修改。
- 当前快速 profile 每节点使用一个 3 GHz AtomicSimpleCPU 和 1 GB DDR3-1600。
- 两个独立 400 Gbit/s 端口经显式 L1 switch 做 0 对 0、1 对 1 映射。
- 链路传播延迟和保守同步量子均为 100 ns；未添加用于拟合实测的固定 service delay。
- 正式 READ/WRITE capability 为 64 KiB；1 MiB 单 WQE 仅作为机制性诊断。
- UBAGG balance 使用两个 primary plane；`standalone` 不提供相同的双路径调度。

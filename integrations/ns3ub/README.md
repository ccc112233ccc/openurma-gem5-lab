# ns-3-UB 集成组件

本目录维护 UB-NET 到原生 ns-3-UB 对象的适配器，以及项目自有的补丁、模型政策和契约测试。主机、设备、网络三层的正式边界见[设备与网络模型](../../docs/ns3ub-network-boundary.md)；旧版补丁过程与实验描述保存在[历史材料](../../docs/validation/ns3ub-readme-history-20261021.md)，不作为当前默认配置。

## 运行职责

[`ub-net-adapter.cc`](ub-net-adapter.cc)接收独立 UDMA 进程发来的完整 WQE 封套，重组可能存在的 IPC 环分片，然后交给原生 `UbJetty` 和 `UbCtpTransportService`。它建立 `UbController`、`UbFunction`、`UbTransaction`、端口、交换机或直连链路，并把目标端内存动作委托回相应 UDMA 进程。原生 CTP 端点负责 4 KiB 上限分段、TASSN、顺序、窗口和可配置重传；交换路径负责转发、序列化、排队与链路状态。

设备外部边界时延和交换处理时延分开配置。交换拓扑的端点 `latency_ns` 计在各自的 UB-NET 边界，`delay_ns` 对每次交换转发计一次；内部 `UbLink` 不再加同一传播延迟。直连任务只建立一条 `UbLink`，不实例化交换节点。详见[同步与计时](../../docs/synchronization.md)及[拓扑编排](../../docs/topology-orchestration.md)。

## 构建与验证

```sh
./lab build ns3ub
```

针对分段、顺序、丢包重试和反馈的局部契约位于 [`tests/`](tests/)；它们验证模型边界，不能代替带官方驱动的全系统性能校准。补丁位于 [`patches/`](patches/)，固定的上游来源见仓库根目录 [`SOURCE_REVISIONS.md`](../../SOURCE_REVISIONS.md)。启动参数应由 `./lab topology plan FILE.json` 与运行 manifest 核对，避免把单次实验开关误认为默认行为。

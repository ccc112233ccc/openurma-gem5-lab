# UDMA 与 ns-3-UB 的设备网络边界

## 边界原则

每台机头外有一份独立 UDMA 设备进程。机头通过 `UB-HOST` 提供客体物理内存、MMIO 和中断；UDMA 通过 `UB-NET` 与一个 ns-3-UB 网络进程相连。每项队列、延迟和状态机应在一侧计入一次，避免 CPU、设备和网络重复收费。

| 行为 | 负责组件 |
| --- | --- |
| 官方 provider、OLK UB/UMMU/UDMA 驱动 | gem5/QEMU 客体 |
| 设备发现、寄存器、管理队列、资源与 TP/Jetty 上下文 | UDMA 模型 |
| SQE/RQE/CQE、门铃、令牌与地址翻译、客体内存 DMA、MSI | UDMA 模型 |
| WQE 解析、源物理端口选择、完整操作封套 | UDMA 模型 |
| CTP 操作分段、TASSN、顺序、窗口和可选请求重传 | ns-3-UB 原生端点/Jetty |
| 链路状态、端口序列化、交换队列、仲裁、转发和故障 | ns-3-UB |
| 目标内存动作、发起端 CQE 生成 | 对应端的 UDMA 模型 |

`UB-NET` 传输的是完整 WQE 的网络封套，不传递主机 DMA 请求或内部 CQE 状态。封套若超过共享内存环单条记录可承载的大小，IPC 会分片，但这种分片不等同于 CTP 包。ns-3-UB 端点将封套转为原生 CTP/UPI/EID/TA 数据流；交换机只按端口和目的地址转发，不解析 WQE 或访问客体内存。代码入口为 [`ub-net-adapter.cc`](../integrations/ns3ub/ub-net-adapter.cc)、[`protocol/ub_net`](../protocol/ub_net/) 与独立[设备进程](../components/udma-device-sim/)。

## CTP 操作生命周期

- SEND 当前按单包操作处理，超过 4 KiB 传输 MTU 的请求会被拒绝。READ 和 WRITE 则以最多 4 KiB 的**有效载荷段**执行；不把 2 B、128 B 等小操作填充成 4 KiB 有效数据。
- 原生 Jetty 为请求段分配 TASSN，维护窗口和 NO/RO/SO 顺序状态。READ 响应与 WRITE 确认带回对应请求的序号和偏移。所有段完成后，原生传输服务通知发起端 UDMA，由其产生官方格式的 CQE。
- 目标端 UDMA 对每段分别执行地址校验、翻译与 DMA。重放时目标端应抑制重复 WRITE/READ 内存动作，并重放相应确认或响应。请求丢失、确认丢失、READ 响应丢失和重试耗尽均有局部契约测试。
- CTP 请求超时重传是**可配置的模型功能**；当前声明式启动器的默认配置为 `off`，即按无丢包下层运行。启用重传时，超时和预算在虚拟时间中计算。拥塞反馈标记与恢复速率也属于可配置的实验模型政策，不能据此推断真实设备的具体算法。

这些规则说明现有实现方式，不宣称覆盖公开 URMA/CTP 的全部协议细节。模型审计与尚未验证部分见[网络模型审计记录](validation/ns3ub-rma-model-audit-20261010.md)。

## 物理端口与延迟

网络适配器用原生 `UbSwitch`、`UbPort`、`UbLink` 连接多个设备端口，或在两节点直连任务中仅建立一条 `UbLink`。EID 选择目标端点，物理端口号选择该端点的入口或出口；控制握手用的 IP 地址不参与 UB 数据转发。

交换拓扑中，端点到网络的单向传播延迟位于 `UB-NET` 边界，交换处理延迟在每次交换转发时计一次。内部 `UbLink` 的传播延迟为零以避免重复。网络还独立计入端口序列化、队列和仲裁。因此配置的固定延迟之和并非完整 `read_lat`。直连没有交换处理，单条链路的单向时延分摊到两端边界。

## 同步与链路状态

`FRAME` 与 `SYNC` 在每个端口的 FIFO 中保持顺序；`SYNC` 公告安全时间界，不携带 EID、Jetty 或工作负载身份。同步开启时，网络进程只能推进到全部输入链路允许的最早安全时刻；背压会阻止生产者越过尚未交付的数据。同步的启停由启动器按运行模式决定，gem5 `modern` 的 ROI 生命周期与 QEMU TCG `icount` 的区别见[仿真同步与计时](synchronization.md)。

`LINK_STATE` 由网络边界向 UDMA 报告端口状态。UDMA 保留 TP 到物理端口的选择状态，并对不可用端口作相应处理。链路速率、延迟、交换处理时间和端口数由[拓扑 JSON](topology-orchestration.md)给出；正式性能报告必须保留解析后的 manifest。

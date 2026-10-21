# UDMA 设备模型

## 代码结构

[`components/udma-model`](../components/udma-model/)实现不依赖 gem5、QEMU 或 ns-3 类型的设备行为；[`components/udma-device-sim`](../components/udma-device-sim/)把模型放入独立进程，并通过 `UB-HOST` 与 `UB-NET` 接到机头和网络。模型通过 `HostInterface` 发起 DMA/中断，通过 `NetworkInterface` 收发网络封套，由 `AdvanceTime` 接收虚拟时间。时间同步和进程背压由设备进程及相邻适配器共同处理，详见[同步说明](synchronization.md)。

实现已按设备发现、控制命令、资源、SQ、完成、地址翻译、运行状态和接收路径拆为独立编译单元；逐文件职责见[源码结构](../components/udma-model/README.md)。

## 控制平面

设备模型暴露官方驱动探测所需的 UBIOS/UBC/UMMU/UBASE 寄存器与发现信息。它以异步 DMA 处理 UBIOS 与 UBASE 控制队列、响应、索引和中断，维护 JFC、JFR、JFS、Jetty、TP 等资源上下文。UMMU 路径处理令牌、地址转换及模型内 IOTLB；MSI 使用客体配置的地址/数据经平台中断路径交付。物理端口链路状态影响 TP 路由和 WQE 发出。

这些是对现有官方 OLK 运行路径的**实现子集**。未使用到的 UBASE/邮箱命令可能只返回基本完成，部分修改/查询、权限和故障上报语义尚未完整覆盖。成功探测到 `udma0` 不能推导出所有官方硬件特性可用。逐操作审计见[官方驱动覆盖记录](validation/official-driver-coverage.md)。

## 数据平面

官方 provider 写 SQE、更新队列并敲门铃后，模型按官方格式读取与解析 WQE。SEND 消耗对应接收端 RQ；WRITE 读取源载荷并在目标执行写 DMA；READ 在目标执行读 DMA，再把响应写回发起端客体内存。完成后模型写官方格式 CQE，并根据 JFC/CEQ 设置触发通知。对于非内联 WRITE，源载荷 DMA 可按每 Jetty 的模型窗口并行准备，最终网络发出与完成仍受顺序和资源状态约束。

设备只把完整 WQE 操作封套交给 `UB-NET`；CTP 的 4 KiB 分段、TASSN、顺序与可选重传在原生 ns-3-UB 端点中。网络返回后，设备再决定官方 CQE/错误状态。详见[设备网络边界](ns3ub-network-boundary.md)。

## 容量、状态与可观测性

队列深度、SQ 预取、READ 在途窗口、WRITE 源 DMA 窗口、IOTLB 容量与 Jetty 上限均是模型参数。它们不应被解释为未测得的真实设备定值。当前实验所用 47 个 Jetty 身份来自目标硬件上限信息，模型的标准/公共 Jetty 拆分仍需与实机能力报告逐项核对。性能报告必须记录这些参数，而不能只写消息大小与端口速率。

设备进程日志和模型计数器可用于分辨 SQ 预取命中、DMA 等待、READ 窗口停顿、队列拒绝、网络背压、完成发布和错误路径。定位吞吐瓶颈时，先看工作负载总操作数、Jetty 数、客体 CPU profile 与启动/预热方式，再结合这些内部计数和网络端口统计。历史诊断方法与结果位于[验证资料](validation/README.md)，不作为新配置下的默认性能结论。

设备进程与 gem5 客体的协调 checkpoint 需要保存相容的设备状态；忙碌中的异步 DMA 与通知不能仅靠复制内存文件安全恢复。启动器按 manifest 与设备状态版本检查兼容性。节点模板 checkpoint 在设备初始化前生成，因此不共享运行中的 UDMA 状态。

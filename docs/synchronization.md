# 仿真同步与计时

## 时间边界

机头、UDMA 和 ns-3-UB 是独立进程。`UB-HOST` 与 `UB-NET` 使用带时间戳的数据消息和 `SYNC` 消息，按相邻端口的 FIFO 顺序交付。发送方承诺不会在已公告的安全时间之前补发消息；接收方只能推进到各输入端口允许的最小时间界。正的边界时延提供保守同步所需的前瞻量。排队、背压和未完成的输出会约束本进程继续推进。此机制不使用宿主机墙钟充当仿真延迟，也不按 `urma_perftest` 的连接对建立全局屏障。

## 两种运行方式

| 配置 | gem5 声明式任务 | QEMU 声明式任务 | 结果用途 |
| --- | --- | --- | --- |
| `execution.sync: required` | `modern` 配置快速启动；进入 perftest ROI 时将 CPU 切至 ArmO3，并通过生命周期围栏启用跨进程同步；离开 ROI 后排空并切回启动 CPU | TCG `icount` 与各进程边界同步，从启动时运行在共同虚拟时间轴 | 有明确模型参数的计时实验；仍须与实测校准 |
| `execution.sync: off` | 不启用该生命周期同步 | 默认不用 `icount`，机头与设备/网络没有共同虚拟时间轴 | 启动、驱动和数据正确性检查 |

gem5 的同步状态由 [`ubsim_lab/config_runtime.py`](../ubsim_lab/config_runtime.py)解析，`modern` 配置的同步量子是 `min(100 ns, 最小端点链路时延)`，外部 UDMA 主机边界时延与该量子配套；启动器把生命周期参数交给 gem5、UDMA 和 ns-3-UB。**当前声明式 gem5 ROI 计时路径不是从冷启动第一拍就让所有进程进行细粒度同步**。其他 profile 与旧命令行路径以实际运行 manifest 为准。

QEMU 同步计时由 TCG `-icount` 和边界 `SYNC` 共同构成。仅启用 `icount` 而关闭进程间同步仍不能得到可比的 UB 时延；`UBSIM_QEMU_ICOUNT_WITH_SYNC_OFF=1` 只供对照诊断。QEMU 的功能模式输出不应解释为实机微秒延迟或带宽。详见[同步开关对照记录](../results/qemu-sync-compare-20261021/README.md)。

## 网络延迟只计一次

交换拓扑中，JSON 链路的 `latency_ns` 是该端点到网络的**单向**传播时延，计在相应 `UB-NET` 边界；交换机的 `delay_ns` 在每次经过交换处理时计一次。ns-3-UB 适配器内部 `UbLink` 的传播延迟设为零，以免重复计算。一次 READ 往返会经过请求和响应两个方向，不能把交换时延只算成整次操作的一笔费用。排队、序列化、目标 DMA、发起端 CPU/设备处理仍会增加延迟。

直连拓扑没有交换处理。其单条链路的 `latency_ns` 表示完整单向传播时延，启动器把它拆分到两端 `UB-NET` 边界。画板显示的固定路径时延因此仅是传播与交换处理参数的和，不等于 `read_lat`。

## checkpoint 与有效性

checkpoint 保存的是 gem5 客体/设备状态，不是对任何未来网络拓扑的性能承诺。无身份的节点模板在驱动初始化前保存，可在机器和镜像指纹兼容时实例化多个节点；逐节点协调 checkpoint 则包含已初始化的 UDMA 状态，恢复时需要对应节点状态和端点参数匹配。网络进程按本次拓扑重建。规则见[节点模板 checkpoint](node-template-checkpoint.md)。

发布计时结果至少保存：拓扑 JSON、解析后的 manifest、镜像及 checkpoint 来源、同步模式、客体工作负载参数、原始 UART/日志与统计文件。首次恢复与后续运行的 CPU 缓存模式和预热状态可能不同，比较前应统一启动与预热流程。

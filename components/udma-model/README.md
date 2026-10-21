# UDMA 模型源码结构

`UdmaModel` 保持一个对外接口和一份设备状态；实现按职责拆成多个编译单元。项目的设备行为、仿真边界与性能适用范围见[UDMA 设备模型说明](../../docs/udma-device.md)。

| 文件 | 主要职责 |
| --- | --- |
| `include/ubsim/udma_model.h` | `HostInterface`、`NetworkInterface`、设备配置、状态及公开接口 |
| `include/ubsim/udma_abi.h` | 与官方队列交互所需的格式常量 |
| `src/udma_model.cc` | 构造、设备发现表、MMIO 和 UMMU 命令入口 |
| `src/udma_control.cc` | UBIOS、UBASE、CtrlQ 的请求与响应 |
| `src/udma_resources.cc` | 邮箱、资源上下文、AEQ/CEQ/JFC 生命周期及 Jetty 门铃 |
| `src/udma_sq.cc` | SQ 预取、WQE 解析、READ/WRITE/SEND 提交与有序退休 |
| `src/udma_completion.cc` | CQE 发布、完成事件、通知与 MSI |
| `src/udma_translation.cc` | UMMU/令牌、MAPT、页表遍历和客体 DMA |
| `src/udma_runtime.cc` | 测试 ABI、链路状态、时间推进、进度与 checkpoint |
| `src/udma_receive.cc` | 远端 READ/WRITE 请求与响应、SEND 接收队列 |
| `src/udma_model_internal.h` | 实现文件共用的内部常量和编码/序列化模板；不是公开 ABI |

这个拆分只调整代码组织，不在文件边界增加进程、队列或额外的仿真延迟。`components/udma-device-sim` 仍通过同一个静态库连接模型。新增状态或容量参数应先放入 `UdmaModel::Config`，同时核对启动器传参、checkpoint 兼容性和对应契约测试。

在仓库根目录验证：

```sh
./lab build udma-model
./lab build udma-device
```

前一命令编译模型并运行本目录的 CTest 套件；后一命令验证独立设备进程及其进程间契约。

# gem5 KVM 功能模式

`kvm` profile 使用 gem5 的 `ArmV8KvmCPU` 加速客体指令执行。UDMA、UMMU、中断、队列以及 ns-3-UB 网络仍走本项目的独立模型。该模式的客体内存行为以快速功能执行为主，适合启动、驱动与数据正确性检查，不用于 CPU/cache 性能预测。

## 条件与运行

需要 ARM64 Linux 宿主机、可读写的 `/dev/kvm`，以及启用 ARM KVM 的 gem5 构建。启动器在进程创建前核对宿主机架构、KVM 设备及构建配置。当前 KVM 路径限制为一个 vCPU；多 vCPU 与 UB-HOST 事件队列尚未验证。

```sh
./lab start --profile kvm
```

默认 `UBSIM_SYNC=auto` 在 KVM profile 中关闭跨进程保守同步，以减少频繁的 KVM 退出。此时仍使用相同的数据接口和设备模型，但不同进程没有共同虚拟时间轴；客体输出的跨进程微秒数不应作为模型计时结果。`--sync` 可用于集成诊断，短前瞻量会显著增加退出开销。实际解析值应检查运行 manifest 的 `virtual_time_synchronization`。

## 平台限制

KVM 的客体 CPU 特征寄存器来自宿主机，而模型 UMMU 的能力字段由项目配置给出。若宿主机地址宽度/ASID 宽度超过模型 UMMU 的能力，OLK 会关闭 SVA 支持，官方 UDMA 初始化可能在创建 `udma0` 前返回 `-ENODEV`。这不能单独证明 UB 数据路径错误。需要可移植的功能对照时，使用 `fast` profile 与官方 provider，并检查驱动日志、能力字段和运行 manifest。

KVM 客体计时器拓扑与解释执行的 gem5 CPU 不同；平台配置会保留适用的 MMIO 计时器，避免两个计时器接口同时不可见。macOS 不支持本工程的 gem5 KVM 路径。

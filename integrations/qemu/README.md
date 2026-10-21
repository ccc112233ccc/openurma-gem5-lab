# QEMU UB-HOST 适配器

本目录只实现 QEMU 客体平台与独立 UDMA 设备进程之间的 `UB-HOST` 接口。它映射设备 MMIO，转发客体物理内存 DMA 与平台中断，并处理同步所需的虚拟时间。WQE、Jetty、RMA 和网络行为仍在独立 UDMA 与 ns-3-UB 进程中，不在 QEMU 中复制。完整系统图见[系统架构](../../docs/architecture.md)。

## 启动方式

QEMU TCG 有两种任务模式：

- `execution.sync: off`：功能模式，默认不使用共同 `icount` 时钟，用于驱动探测、数据正确性与回归；客体打印的微秒数不作为模型时延。
- `execution.sync: required`：TCG `icount` 与 `UB-HOST`、`UB-NET` 的时间戳及 `SYNC` 一起开启，从启动阶段保持共同虚拟时间轴。它可用于受同步约束的模型实验，但数值仍需单独校准。

对照诊断用的 `UBSIM_QEMU_ICOUNT_WITH_SYNC_OFF=1` 可以只保留 QEMU `icount` 而关闭进程同步；这不是可用于 UB 时延结论的模式。详细规则见[仿真同步与计时](../../docs/synchronization.md)。

已有工作区的两节点功能任务可按下列方式运行：

```sh
./lab topology validate configs/topology-qemu-two-heads.json
./lab topology plan configs/topology-qemu-two-heads.json
./lab topology start configs/topology-qemu-two-heads.json
./lab status-qemu
./lab qemu-rma-regression
./lab stop-qemu
```

QEMU 声明式任务支持 2–8 台主机的单交换机拓扑或两节点直连，可给不同机头设置 CPU 类型、核数和内存；目前不支持 checkpoint 恢复。可执行文件与客体镜像的构建入口见[编译与运行](../../docs/build-and-run.md)。QEMU 的 HVF 与当前 GICv2m MSI 桥接路径不匹配，官方 UDMA 客体路径以 TCG 为准。

## 代码与验证

此目录中的适配代码配合项目固定版本 QEMU 补丁构建。功能回归记录见[QEMU 验证材料](../../docs/validation/README.md)。模拟器启动时保存的 manifest 是判断功能/计时模式、端口速率与边界时延的依据。

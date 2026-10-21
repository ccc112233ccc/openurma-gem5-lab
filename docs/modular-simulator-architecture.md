# 模块化仿真架构

本页保留原文档路径，供已有链接使用。当前实现的正式说明已整理为以下相互独立的章节：

1. [系统架构](architecture.md)：进程组成、UB-HOST/UB-NET、操作路径和源码归属。
2. [UDMA 设备模型](udma-device.md)及[设备与网络边界](ns3ub-network-boundary.md)：控制/数据平面、CTP 和延迟职责。
3. [仿真同步与计时](synchronization.md)：gem5 ROI 生命周期、QEMU 计时模式和同步规则。
4. [官方软件栈集成](guest-driver-integration.md)：OLK、UMDK、initramfs 与模拟器适配器的关系。

早期架构笔记曾把 CTP 分段和重传归在 UDMA 一侧，并把 gem5 的同步描述为总从启动时刻开启；这些说法不适用于当前声明式 `modern` 路径。以以上章节和当前源码为准。

# 工程参考入口

本页保留旧参考手册的地址，避免已有链接失效。现行工程说明从[文档目录](README.md)进入：

- [系统架构](architecture.md)：进程及组件职责。
- [设备与网络边界](ns3ub-network-boundary.md)：WQE、CTP、端口和交换路径。
- [UDMA 设备模型](udma-device.md)：控制面、队列、DMA、CQE 与资源参数。
- [仿真同步与计时](synchronization.md)：可解释的计时条件。
- [官方软件栈集成](guest-driver-integration.md)：客体驱动与镜像构建。
- [编译与运行](build-and-run.md)：构建、启动、checkpoint 和日志。
- [拓扑编排](topology-orchestration.md)：JSON 参数、直连与交换、多节点限制。
- [验证资料](validation/README.md)：保留实验的原始背景和结果。

命令接口以 `./lab --help` 和当前源码为准；历史文档中的命令、性能数字和默认值可能属于旧配置。

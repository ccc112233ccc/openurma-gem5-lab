# 原生 CTP 端点验证

当前设备网络边界见[设备与网络模型](ns3ub-network-boundary.md)：UDMA 经 UB-NET 提供完整 WQE 封套，原生 ns-3-UB 端点处理 CTP 分段、顺序和可选重传，目标 UDMA 执行客体内存动作。该说明是现行接口的参考。

早期外部端点迁移、局部契约和功能回归的原始记录保存在[历史验证材料](validation/ctp-external-endpoint-history.md)。其中的参数与结果只对应记录时的源码和镜像。

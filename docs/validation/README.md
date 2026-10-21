# 验证资料索引

本目录保存实验时点的假设、参数、日志路径和结果。它是可追溯证据，**不是**当前实现规范、默认参数表或已承诺的性能精度。阅读前先看[正式文档目录](../README.md)，再核对对应实验的镜像、checkpoint、代码版本、拓扑、同步模式和操作次数。

## 常用证据入口

| 主题 | 记录 |
| --- | --- |
| 官方驱动与硬件服务覆盖 | [驱动覆盖审计](official-driver-coverage.md) |
| 原生 ns-3-UB 的 RMA 模型边界 | [网络模型审计](ns3ub-rma-model-audit-20261010.md) |
| gem5 本机运行与同步冒烟测试 | [原生 gem5 验证](gem5-macos-native-20261020.md) |
| 64 KiB 客体 READ 曲线 | [全曲线记录](gem5-macos-64k-read-curve-20261020.md) |
| UDMA 模型拆分后的同步性能复测 | [gem5、QEMU 与实机对照](udma-refactor-sync-read-20261021.md) |
| 直连、交换及非对称链路 | [gem5 拓扑](topology-gem5-20261020.md)、[直连](gem5-direct-link-checkpoint-20261020.md)、[非对称链路](topology-asymmetric-links-20261020.md) |
| 多节点及模板 checkpoint | [三节点恢复](gem5-3node-memory-checkpoint-20261020.md)、[模板说明](../node-template-checkpoint.md) |
| QEMU 多节点与直连 | [三节点](qemu-3node-orchestration-20261021.md)、[直连](qemu-direct-link-20261020.md) |

`results/` 保存已提交的汇总数据或图，`experiments/` 在本机保存运行目录和原始日志，不随 Git 推送。实验文档中的“当前”只指其记录日期；有冲突时，以明确的运行 manifest 和当前源码为准。

早期英文首页、组件说明与会话交接材料已原样归档在本目录的 `*history*.md` 文件中。它们保留原文和当时的参数以便追溯；正式说明统一使用上面的中文文档。

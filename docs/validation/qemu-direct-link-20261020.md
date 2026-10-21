# 双节点 UB 直连验证（2026-10-20）

## 结论

QEMU 双机、两台独立 UDMA、一个 ns-3-UB 网络进程在**无中央交换节点**的拓扑下完成了官方 URMA API 数据完整性验证。网络进程仍负责两个 `UB_DEVICE` 的 CTP 协议与事件推进，但两个端口接到同一个 `UbLink`，没有创建 `UB_SWITCH`。这与把交换处理延迟设为零不同。

## 配置与运行

- 配置：[`configs/topology-qemu-direct-link.json`](../../configs/topology-qemu-direct-link.json)，两个节点各一个 400 Gbit/s UB 端口，直连链路声明 750 ns 单程传播；适配器在两端 UB-NET 边界各配置 375 ns，内部 `UbLink` 延迟为零，避免重复计时。
- `./lab topology validate configs/topology-qemu-direct-link.json` 与 `plan` 成功，计划中的 `switch_id` 为 `null`、`fabric_layout` 为 `direct`。
- 原生 macOS `./lab topology start ...` 启动五个进程，两台客体到达 shell；网络日志记录 `topology=direct ports=1`。
- 完整 `./lab build ns3ub`（含原有协议进程检查）通过，用时 43.76 秒；双机启动命令用时 0.72 秒。
- QEMU 使用 `--functional`，没有同步计时；本轮仅验证功能，不声称直连延迟或带宽精度。

## 数据完整性

`tools/run_qemu_integrity.py` 的 `smoke` 三项全部通过：4 KiB SEND、WRITE、READ；`large` 四项全部通过：65,537 B 和 1 MiB 的 WRITE、READ。每项均检查两端的接受标记和载荷，完整报告分别在 `experiments/qemu-direct-link-20261020/integrity/report.json`、`experiments/qemu-direct-link-20261020/large/report.json`，UART 原始输出与运行日志在同一实验目录。
两组负载分别用时 6.92 秒和 9.20 秒。

正常停止后的 ns-3-UB 统计：`native_wqes_submitted=28`、`native_wqes_completed=28`、`native_wqes_failed=0`、`ns3_runtime_drops=0`、`ctp_retransmission_exhausted=0`。两台 UDMA 均为 `sq_wqes=14`、`sq_completions=14` 且 `quiescent=1`。实验结束后五个模拟进程均停止。

## 边界

当前直连模式限定为两台设备、每台一个端口、相同端口速率。交换模式仍支持原有多节点/多端口拓扑。后续已用现有交换拓扑 shell-ready checkpoint 在 gem5 直连网络下恢复并完成同步短测，见 [gem5 直连 checkpoint 验证](gem5-direct-link-checkpoint-20261020.md)。直连与交换模式的延迟、带宽精度比较需要在同等负载下另测。

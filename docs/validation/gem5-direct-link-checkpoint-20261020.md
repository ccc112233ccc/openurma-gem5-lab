# gem5 直连网络复用交换拓扑 checkpoint（2026-10-20）

## 结论

同一套 gem5 客体和 UDMA 设备配置，交换拓扑与直连拓扑**不必各自创建 checkpoint**。本次用已有交换拓扑的 64 KiB shell-ready checkpoint，在无中央交换节点的 ns-3-UB 直连网络下恢复两台 gem5 客体，完成同步计时与 5/5 项 URMA 短测。此前启动器因比较 `ub_fabric_layout` 和 `peer_switch_delay` 而拒绝此 checkpoint；这是选择规则过严，并非 gem5/UDMA 恢复格式要求。

## 原因与兼容边界

`scripts/run/checkpoint-dual.sh` 保存两台 gem5 客体状态和两个 UDMA 设备状态文件，没有保存 ns-3 网络进程的交换节点、链路或事件状态。协调 checkpoint 在客体 shell-ready、UDMA 无在途操作时创建；恢复时重新启动网络进程并按本次拓扑建网。因此，保持 gem5 机器、内核与 initramfs、UDMA 端口数与 EID、端点链路速率与传播延迟等设备参数一致时，可以改变网络布局及交换处理延迟。启动器现在不再用这两个纯网络参数筛除 checkpoint，其他兼容性检查仍保留。

这里复用的是**协调、静止状态** checkpoint。不能由此推断任意业务运行中截取的状态都可以跨拓扑恢复；那种情形需要保存并恢复在途网络状态，或者先排空网络。

## 本次配置与证据

- 拓扑：`configs/topology-gem5-direct-link.json`；两台 modern-profile gem5，各一台 UDMA、一个 400 Gbit/s 端口，EID 256/257，LQ/SQ=64/64、`stride_exclusive`。一条 750 ns 单程直连链路分配为两端各 375 ns UB-NET 传播延迟，中央交换节点数为零。
- 使用 `UBSIM_KERNEL=artifacts/kernel-64k-20261015/vmlinux`、`UBSIM_INITRD=out/official-udma-64k-20261015.cpio.gz`（均为项目根目录下路径）；`./lab start-ready --print-config` 选中既有 `shell-ready-modern-ddr5-stock-64k-lq64-sq64-link375-switch500-20261015`。实际运行的 `experiments/gem5-direct-link-20261020/run/run-manifest.txt` 同时记录 `restore_checkpoint` 为该旧 checkpoint、`ub_fabric_layout=direct`、`peer_switch_delay=0ns` 和两个端点各 400 Gbit/s、375 ns。
- 原生 macOS 执行 `./lab topology start configs/topology-gem5-direct-link.json`，启动命令用时约 0.44 秒；`./lab sync` 通过，两台客体的架构计时器均可用，用时约 2.54 秒。
- `./lab rma-regression --sync-smoke experiments/gem5-direct-link-20261020/sync-smoke`：128 B SEND/WRITE/READ 时延、128 B SEND 带宽、4096 B READ 带宽共 5/5 项成功；每项 `validation_errors=[]`，两台 gem5、两台 UDMA、网络进程均有同步栅栏证据。报告为 `experiments/gem5-direct-link-20261020/sync-smoke/report.json`，测试套件用时约 51.45 秒。
- ns-3 日志 `experiments/gem5-direct-link-20261020/run/ub-switch/gem5.log` 启动行写明 `topology=direct ports=1`；结束统计 `native_wqes_submitted=184`、`native_wqes_completed=184`、`native_wqes_failed=0`、`ns3_runtime_drops=0`、`ctp_retransmission_exhausted=0`。
- 测试后执行 `./lab stop`，仿真进程已停止。

本次验证 checkpoint 可复用、直连功能与同步计时路径可运行。短测操作次数较少，不能用来判断直连和交换拓扑的实际时延、带宽差值或与实机的精度。

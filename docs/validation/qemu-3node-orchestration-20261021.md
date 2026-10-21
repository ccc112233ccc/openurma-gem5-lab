# QEMU 三主机编排验证（2026-10-21）

本机用 `./lab topology start /tmp/ubsim-qemu-three-smoke.json` 启动三台各 768 MiB、单核 Cortex-A72 的 QEMU 客体。拓扑为三台独立 UDMA，经 ns-3-UB 单交换芯片连接；每端单端口 400 Gbit/s、端点链路 375 ns、交换处理 500 ns。启动前内存检查显示宿主机 16.0 GiB 中可用 8.5 GiB，保留线 3.2 GiB。启动到停止约 101 秒（运行清单 15:02:47，控制网进程退出 15:04:28）。运行时内存保护记录的最低可用内存为 7.7 GiB，未触发停止。

三台 QEMU、三台 UDMA、ns-3-UB 和控制网学习转发进程均启动；三台客体都进入 shell，`udma0` 和各自的 EID 0x100、0x101、0x102 均为 ACTIVE。第三台的 `ping -c 2 10.0.0.1` 为 2/2 收到。第一台与第三台运行 `urma_perftest read_lat`，128 B、32 次、1 Jetty、CTP，双方退出码均为 0，第三台生成完整结果行。QEMU 功能模式中的 774.74 µs 平均值仅证实数据通路执行成功，不能与 gem5 计时或实机延迟比较。

`./lab stop-qemu` 停止了三台 QEMU、三台 UDMA 和 ns-3-UB；控制网转发进程在客体断开时自行退出。之后 `./lab status-qemu` 显示所有进程已停止。工作台服务重启后，`/api/topology/checkpoint` 对 QEMU 三主机返回可启动及三个主机配置。运行记录保留在 `run-qemu-dual/`，其 `run-manifest.txt` 含 `node_count=3`、逐节点 UART/SSH/OOB 端口和 `oob_backend=learning-relay`。

本轮仅实际启动三主机；四到八主机经过拓扑与端口分配检查，没有在 16 GiB 宿主机上启动以避免内存压力。多主机内置性能实验配方仍限双机，其他节点可从各自终端执行命令。

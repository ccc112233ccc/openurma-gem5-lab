# 128 B READ：逐语句 profiling 与 Jetty 对照（2026-10-14）

## 结论

前一轮 J=1→J=2/4 的显著吞吐下降，大部分是测试程序的**自动计时模式切换**造成的，不能归因于 Jetty、链路或 UDMA 资源本身。`perftest_parameters.h` 将 `PERFTEST_BW_NO_PEAK_INTERS` 定为 20000；`perftest_parameters.c` 在每 Jetty 的 `cfg->iters > 20000` 时自动设置 `no_peak=true`。此前固定总操作数 32768，故 J=1 的每 Jetty 32768 次关闭逐操作峰值计时，而 J=2/4 的 16384/8192 次仍对每次提交和完成调用 `perftest_get_time()` 并写时间戳数组。该条件来自测试程序参数逻辑，不是硬件 Jetty 特性。

统一显式 `-N` 后，J=1 两次为 10.56/10.69 Mops/s，J=2 为 10.34，J=4 为 10.01。J=4 相对 J=1 均值下降约 5.8%，远小于原来表面上的约 17%。逐语句计时显示，CQE 记账在同一模式下约 76–82 µs，未随 Jetty 数量暴涨。保留峰值计时并固定总操作数 16384（与提供的物理机 `-n 16384` 一致）时，J=1 两次为 9.79/9.85，J=2 为 9.40，J=4 为 9.64 Mops/s；多 Jetty 的小差别存在运行波动，尚不能判定稳定收益或稳定退化。

当前与物理机的**小包 READ 绝对性能差距仍很大**：原版 `urma_perftest`、J=1、128 B、`-n 16384` 测得 9.606 Mops/s（1172.60 MiB/s）；提供的实测为 23.098 Mops/s（2819.64 MiB/s）。模拟值约为实测的 41.6%，未达到期望的 80%。

这只是关键负载参数的对照，不是硬件逐项等价：模型使用 4 核、3.2 GHz 通用 ArmO3、4-KiB guest 页，而实机是 2.3 GHz Kunpeng 950、64-KiB 基础页和不同的真实 I/O/缓存结构。实测还来自同进程的包长扫描；本实验每组重新启动 perftest 进程。以上差异都可能影响剩余的绝对值误差，需要逐项校准。

## 方法与数据

从 `shell-ready-modern-ddr5-jetty47-perjetty-20261010` checkpoint 恢复两节点，ROI 使用 ArmO3，官方 UMDK/CTP/RM 栈，单个 UB 物理端口 `-p 0`、400 Gbit/s、交换芯片单次 500 ns。固定总在途 JFS/JFR 深度 8192，J=1/2/4 分别为每 Jetty 8192/4096/2048；`-l 256 -Q 1 -C 524288 -I 0`。只更改 Jetty 数、每 Jetty 次数、用于避免冲突的控制端口和明确说明的 `-N`。各组均经双节点结果一致、退出码 0、五项 ROI fence 检验。

独立编译的 ARM64 `urma_perftest_loop_profile` 在 `run_once_bw` 的四段累加 guest `cntvct_el0` 时间：Jetty 扫描与提交记账（排除 post API）、post API、poll API、完成项处理；进程退出后才输出 `[URMA_LOOP]`。`cntfrq_el0=25,165,824 Hz`。诊断程序只装入客户端 guest 的 `/tmp`，服务器使用原版程序，checkpoint 和 initramfs 未修改。探针不逐操作打印。原版 `-n 16384` 单次 9.606 Mops/s；诊断版同参数两次 9.789/9.846，差异约 2%，计时结果用于定位而非直接校准。

| 总次数 | 计时模式 | Jetty | 速率 Mops/s | 扫描 µs | post API µs | poll API µs | CQE 处理 µs |
| ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: |
| 32768 | 自动 no_peak | 1 | 10.551 | 27.8 | 1463.7 | 1509.2 | 82.8 |
| 32768 | 自动 peak | 2 | 9.111 | 178.0 | 1426.3 | 1497.7 | 473.5 |
| 32768 | 自动 peak | 4 | 8.757 | 200.1 | 1516.8 | 1512.8 | 491.2 |
| 32768 | 显式 `-N` | 1 | 10.562 | 38.5 | 1426.0 | 1540.3 | 76.2 |
| 32768 | 显式 `-N` | 2 | 10.345 | 38.8 | 1458.2 | 1567.8 | 81.8 |
| 32768 | 显式 `-N` | 4 | 10.012 | 41.4 | 1623.7 | 1506.4 | 79.6 |
| 32768 | 显式 `-N` | 1 回归 | 10.692 | 37.6 | 1415.1 | 1507.3 | 82.5 |
| 16384 | 全部 peak | 1 | 9.789 | 42.8 | 619.3 | 767.1 | 234.3 |
| 16384 | 全部 peak | 2 | 9.402 | 64.6 | 646.5 | 789.4 | 231.7 |
| 16384 | 全部 peak | 4 | 9.638 | 74.7 | 633.9 | 749.2 | 232.1 |
| 16384 | 全部 peak | 1 回归 | 9.846 | 66.0 | 602.7 | 751.3 | 233.6 |

32768 次的四组均为 128 次 post、2048 次 poll、每次 16 CQE、0 次空 poll；16384 次四组均为 64 次 post、1024 次 poll、每次 16 CQE、0 次空 poll。全部 CQE 完成。由此，自动模式 J=1→J=2 的约 392 µs CQE 处理增量来自每 CQE 时间戳路径；显式 `-N` 后只余约 6 µs。扫描段中也包括每次 post 的时间戳，自动模式中同样被放大。分段时间是采样边界内的 guest 虚拟时间，不能逐项机械相加成为 `BW average` 的精确倒数（边界和测量点不同）。

物理参数组的 J=1 诊断版 post+poll 合计约 1.37 ms，已经超过物理机 16384 次/23.098 Mops/s 对应的约 0.709 ms；因此即使消除全部测试程序扫描和 CQE 记账开销，仍不足以达到实测。下一步应细分官方 provider 的 post/poll 路径与 gem5 MMIO/DMA 往返，结合 ArmO3 PC 样本和设备队列时间查明额外成本。API 内时间也包含设备完成等待，不能直接解释为纯 CPU 指令成本。

本次最终设备计数为 344064 次 READ WQE 全完成，`read_window_stalls=0`、`sq_depth_rejects=0`、`ubase_errors=0`；交换网络请求/应答各 344064 帧，全数交付，`ctp_wire_pacing_waits=0`、重传/丢包/拥塞降速均为 0。单端口链路速率并未在此 128 B 负载上形成可见瓶颈。模型没有报告有限队列卡住，但不能仅凭这些计数排除 post/poll 内部 MMIO、同步、缓存和 CPU 微架构时延。

## 证据与复现

- 逐语句诊断构建器：`tools/build_perftest_loop_profile.py`。生成文件位于 `artifacts/perftest-loop-profile/`，不会覆盖正式程序。
- 运行驱动：`tools/profile_gem5_read_jettys.py`，新增 `--client-binary`、`--client-env`、`--no-peak`。
- 原自动模式：`experiments/gem5-read-loop-profile-j1-20261014/` 与 `experiments/gem5-read-loop-profile-multijetty-20261014/`。
- 匹配 `-N` 模式：`experiments/gem5-read-loop-profile-matched-nopeak-20261014/`。
- 匹配实测操作数：`experiments/gem5-read-loop-profile-physical-n-20261014/`；其中保存最终设备与网络日志。
- 原版单 Jetty 对照：`experiments/gem5-read-physical-n-stock-20261014/`。

本次 checkpoint 恢复、同步、guest 传输和运行耗时均保存在命令会话输出及各实验的 `results.csv` 的 `wall_seconds`。环境已在收集最终计数后停止。

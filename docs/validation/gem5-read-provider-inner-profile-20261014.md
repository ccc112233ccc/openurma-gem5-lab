# 128B READ：官方 UDMA provider 内部路径 profiling（2026-10-14）

## 结论

本轮继续定位 [逐语句 profiling](gem5-read-loop-profile-20261014.md) 中占主体的 post/poll API 时间。在现有单端口、400 Gbit/s、128B、J=1、`-n 16384`、`-l 256 -Q 1 -I 0`、ArmO3 ROI 配置下，**post API 的主要耗时在逐 WQE 构造，poll API 的主要耗时在逐 CQE 扫描**。两端的单次批量 doorbell 更新及 `dmb st` 不占主要时间。对 CQE 抽样后，owner/地址读取比随后 `dmb ld`、CQE 字段解析更贵；对 WQE 抽样后，`udma_set_sqe` 比输入检查和队列 bookkeeping 更贵。

这些结果进一步缩小了位置，但**不能把 CQE 读取成本直接归结为某一级缓存或 DMA 一致性 bug**：当前探针在最深配置下使吞吐下降约 13–16%，guest 虚拟计时器只有 25.17 MHz，单次样本粒度约 39.7 ns，且不同抽样版本的代码布局与运行顺序会影响数值。原版同会话三次为 9.786、10.211、10.068 Mops/s，存在约 4% 基线漂移。诊断数值用于判断主要代码段，绝对性能继续以原版程序为准。

## 路径与实验

`urma_post_jetty_send_wr` 经 `udma_u_post_jetty_send_wr` 进入 `udma_u_post_sq_wr`：一次提交包含 256 个 WR，逐个调用 `udma_u_post_one_wr` 编码 WQE，然后 `dmb st`，再更新 SQ doorbell。`urma_poll_jfc` 经 `udma_u_poll_jfc`，一次最多扫描 16 个 CQE；`udma_u_poll_one` 先读取 owner 判断有效性，再执行 `dmb ld`、解析 CQE，最后每批更新消费位置。provider 两处代码位于 `sources/umdk/src/urma/hw/udma/udma_u_jfs.c` 和 `udma_u_jfc.c`。

诊断工具 `tools/build_udma_inner_profile.py` 从当前官方 provider 源码复制两份 `.c` 到 `artifacts/provider-inner-profile/`，仅重编这两个对象，再与其余原版对象链接成**独立** provider。客户端 guest 的 `/tmp/profile-lib/liburma.so.0` 是 `/lib/liburma.so.0` 的私有副本；它只从同目录 `/tmp/profile-lib/urma/` 加载诊断 provider。服务器始终使用原版。原始 `/lib/urma/liburma-udma.so`、checkpoint 和 initramfs 均未更改。每个变体在 host/container/guest 之间核对 SHA-256；诊断进程退出时才输出一行 `[UDMA_INNER]`，不逐项打印。

同会话八组**成功**运行均完成 16384 个 READ；其中五组诊断运行都记录 64 次 post、1024 次 poll、每次 16 个 CQE、0 次空 poll。八组的双节点结果与五项 ROI fence 均通过。一次校准探针的预运行因容器 readfile 同名文件与 host 构建产物校验和不一致，客户端在加载时 bus error，**未进入 ROI**；重新 `docker cp` 并核对三层 SHA-256 后重跑成功，该失败组未纳入任何性能结论。

| 顺序 | 客户端 provider | Mops/s | post 总计 µs | 逐 WQE 编码 µs | SQ doorbell µs | poll 总计 µs | 逐 CQE 扫描 µs | CQ 消费位置更新 µs |
| ---: | :--- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 原版 | 9.786 | — | — | — | — | — | — |
| 2 | 批量分段探针 | 8.877 | 595.7 | 574.3 | 17.8 | 879.2 | 848.2 | 5.4 |
| 3 | CQE 1/128 抽样 | 8.919 | 561.0 | 532.0 | 17.3 | 921.1 | 886.3 | 5.4 |
| 4 | 原版回归 | 10.211 | — | — | — | — | — | — |
| 5 | CQE 抽样＋计时器校准 | 8.620 | 599.8 | 570.0 | 17.5 | 935.0 | 901.3 | 5.3 |
| 6 | WQE＋CQE 1/128 抽样 | 8.720 | 602.8 | 581.4 | 17.0 | 925.8 | 894.5 | 5.0 |
| 7 | WQE＋CQE＋SQE 1/127 抽样 | 8.568 | 613.7 | 577.0 | 17.6 | 947.4 | 915.1 | 5.4 |
| 8 | 原版回归 | 10.068 | — | — | — | — | — | — |

批量分段的 post `dmb st` 总计只有约 0.3–0.5 µs/64 次提交。SQ doorbell 约 17–18 µs/64 次；poll 的消费位置更新约 5 µs/1024 次。两个批量 API 内的大头分别是编码循环与扫描循环。未列出的差额含锁、循环控制和计时探针自身成本；`post_lock_calls=64`，`poll_lock_calls=1024`，但本轮没有单独测锁耗时。

### 抽样子段（诊断值）

| 变体 | 样本 | CQE owner/地址读取 | `dmb ld` | CQE 解析 | 相邻计时器空操作 |
| :--- | ---: | ---: | ---: | ---: | ---: |
| CQE 1/128 | 128 | 32.6 ns | 3.7 ns | 12.1 ns | 未测 |
| CQE 1/128＋校准 | 128 | 39.4 ns | 4.3 ns | 10.2 ns | 2.2 ns |
| WQE＋CQE 1/128 | 128 | 25.5 ns | 5.0 ns | 12.4 ns | 1.2 ns |
| 深度 1/127 | 129 | 39.4 ns | 5.2 ns | 12.9 ns | 1.5 ns |

各值是 `128/129` 个抽样所占 guest tick 除以样本数，包含读计时器的成本和约 39.7 ns 的量化误差；不应把某个值当成单次真实延迟。固定每 128 项抽样可能总命中同一队列位置，因此最后一组改为互质的 127 步长。多次结果一致地显示读取 CQE owner/地址高于 `dmb ld` 与解析，但其具体 ns 值仍不稳定。

WQE 1/128 抽样中，输入检查/`udma_set_sqe`/队列指针与 `wrid` 更新约为 6.2/28.6/7.5 ns，计时器空操作 2.5 ns。深度 1/127 版分别约为 7.7/51.4/18.2 ns；深入 `udma_set_sqe` 本身又引入可见扰动，不宜比较这两行的绝对值。深度版的 `udma_set_sqe` 内，溢出检查/SQE 准备（含控制字段清零和远端 EID 拷贝）/WR 字段解析约为 2.8/23.1/11.1 ns，说明 SQE 准备是一个值得进一步核对的源代码段。

### 同一 CQE 的立即重读

为检验首次 owner 读取是否包含缓存首次访问成本，另从同一 checkpoint 恢复一次，使用独立的 `liburma-udma-sampled-relookup.so`。它每 127 次 CQE 检查抽样一次，首次 `get_next_cqe` 后立刻以相同索引再执行一次，保留正常屏障和解析；再次读取的结果仅用于防止编译器删去该访问，不参与完成判定。Arm 对象反汇编确认首次和再次检查各有独立的 `ldr`（样本路径地址约为 `0x9ec` 与 `0xa34`）。

原版/探针/原版三组分别为 9.844/8.665/10.058 Mops/s，探针对吞吐有约 12–14% 扰动。探针的 129 个样本中，首次 owner/地址检查合计 126 tick、立即重复检查 13 tick、相邻计时器空操作 5 tick；按 25,165,824 Hz 计约为每样本 38.8/4.0/1.5 ns。**同一地址的立即二次读取明显较快**，支持首次访问引发额外缓存或一致性层级成本的假设。`get_next_cqe` 的首次路径还读取队列索引、掩码、位移和 CQ 缓冲区地址；此实验尚不能断言慢的是 owner 所在缓存行本身，也不足以区分 L1 miss、L2 命中、DMA 写入引起的失效或 TLB 影响。该探针 ROI 的忙核 L1D demand miss 为 264071 次，累计 miss latency 4,007,104,869 gem5 tick（`simFreq=10^12`），平均约 15.2 ns/miss；这是全 ROI 汇总，不能直接对应到那 129 个 CQE。探针版时间不能当成实机 CQE 读取延迟。本组三组共完成 49152 次 READ，最终 UDMA 和网络计数无窗口停顿、队列拒绝、丢包、重传或 pacing。原始证据在 `experiments/gem5-provider-relookup-stock-before-20261014/`、`gem5-provider-cqe-relookup-20261014/`、`gem5-provider-relookup-stock-after-20261014/`，第二个目录保留最终日志。

## 设备与 gem5 交叉检查

八组共完成 131072 次 READ WQE；最终 UDMA 计数 `read_window_stalls=0`、`sq_depth_rejects=0`、`ubase_errors=0`，网络 262144 个请求/应答帧全部交付，`ctp_wire_pacing_waits=0`，无丢包、重传、拥塞降速。当前数据没有支持“400 Gbit/s 端口或 256 credit READ 窗口压住 128B 包率”的证据。

客户端忙核在原版与批量探针中为 `switch_cpus3`，深度探针中为 `switch_cpus0`，所以按各 ROI 的最大 `numCycles` 选核。原版起点/终点约为 5.47M/5.32M cycles，批量/深度探针约为 6.02M/6.30M；探针确实增加 CPU 周期与 serializing stall，不能用探针总吞吐作实机校准。忙核 L1D demand miss 在原版与诊断组约为 249–271k、L2 data miss 约 36–37k；这支持检查 CQE/WQE 缓存路径，却不能从汇总计数确定哪一条源语句发生 miss。下一步若继续细分应优先对 CQE 内存地址范围记录 cache hit/miss 和 DMA 写入到 CPU owner 读取的间隔，并独立校准 provider 的 SQE 准备路径；避免据此直接调整链路频宽或删除设备内存屏障。

## 证据

- `experiments/gem5-provider-inner-stock-before-20261014/`、`gem5-provider-inner-stock-after-20261014/`、`gem5-provider-inner-stock-final-20261014/`：原版回归。
- `experiments/gem5-provider-inner-profile-20261014/`：批量分段。
- `experiments/gem5-provider-cqe-sample-20261014/`、`gem5-provider-cqe-calibrated-retry-20261014/`：CQE 抽样。
- `experiments/gem5-provider-wqe-cqe-sample-20261014/`、`gem5-provider-deep-sample-20261014/`：WQE 和 SQE 抽样；后者保存最终 UDMA/网络日志。
- `experiments/gem5-provider-cqe-calibrated-20261014/`：SHA 不一致导致的失败预运行，保留 UART 便于复核。
- 构建与运行工具：`tools/build_udma_inner_profile.py`、`tools/profile_gem5_read_jettys.py`。

首轮八组成功 benchmark 的 `wall_seconds` 合计 375.881 s；立即重读的三组为 43.7/47.4/42.5 s。这些数值不含恢复 checkpoint、guest 文件传输和构建时间。所有模拟进程已停止。

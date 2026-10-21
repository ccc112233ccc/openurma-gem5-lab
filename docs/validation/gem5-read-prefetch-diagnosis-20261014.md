# 128B READ 包率：CQE/SQE 预取根因对照（2026-10-14）

## 负载和基线

同一 47-Jetty 上限 checkpoint，但本实验始终使用单端口、单 Jetty、128B、`-n 16384 -l 256 -Q 1 -R 8192 -T 8192 -C 524288`，两端 ROI 同步；实机参考为 23.098464 Mops/s。所有私有 provider 仅加载在发起端，接收端、安装版 provider、网络和 UDMA 配置保持不变。结果按 `experiments/gem5-read-hotpath-*/results.csv` 保存，且双端 UART 行相同、退出码为零，五参与者 ROI fence 验证通过。第一次 checkpoint 恢复的 3.931 Mops/s 是已知预热效应，后续分析使用紧接着的预热 9.938 Mops/s。改动后的原版回归 10.010，末次原版回归 9.611 Mops/s。

## 单因素结果

| CPU 缓存模型 | 发起端 provider | 128B READ Mops/s | 相对 23.098 实机 |
| --- | --- | ---: | ---: |
| 无预取器 | 原版预热 | 9.938 | 43.0% |
| 无预取器 | 移除 `-ftrapv` | 9.860 | 42.7% |
| 无预取器 | CQE 提前 4 项、`prfm pldl3keep` | 14.352 | 62.1% |
| 无预取器 | CQE 提前 8 项 | 14.366 | 62.2% |
| 无预取器 | CQE 提前 4 项、L1 目标 | 14.122 | 61.1% |
| 无预取器 | SQE 提前 4 项、写预取 | 11.268 | 48.8% |
| 无预取器 | CQE 与 SQE 两项预取 | 16.811 | 72.8% |
| L1D stride 预取器 | 原版预热 | 13.681 | 59.2% |
| L1D stride 预取器 | SQE 写预取 | 17.492 | 75.7% |
| L1D stride 预取器 | CQE 与 SQE 两项预取 | 18.015 | 78.0% |

原版 provider 重新链接的 SHA-256 与安装前的原始构建完全一致。私有构建脚本是 `tools/build_udma_hotpath_probes.py`，每个变体只重编 `udma_u_jfs.c` 和 `udma_u_jfc.c`，其他对象和参数不变。AArch64 反汇编确认 CQE 变体实际产生 `prfm`。8 项距离未再增加收益，L1 目标略低；这不是单纯依靠更长 lookahead 就能解决的问题。

## 模型修复及正式回归

`configs/arm64_fs.py` 原先将全部缓存预取器显式设为 `NULL`。对顺序消费 CQE 的负载，这造成每个新 CQE 的首次访问均走缓存 miss/一致性路径。加入可选 L1D `StridePrefetcher(degree=4, distance=2)` 后，**官方 provider** 的预热包率达到 13.681 Mops/s。新缓存对象图通过一次冷启动建立 `shell-ready-modern-ddr5-stride-20261014` checkpoint；协调保存耗时 124 秒，后续恢复使用 `start-ready`。同配置的 4KiB READ 达 46,610.12 MiB/s，实机 45,172.39 MiB/s，误差 +3.2%。128B `read_lat` 的 1024 次短样本均值 2.39µs，实机 3.38µs；固定时延误差仍需单独定位。

gem5 的 `QueuedPrefetcher` 即使由写访问触发也只生成 `HardPFReq`，未要求缓存行写权限；AArch64 软件 `prfm pst...` 则走 `PF_EXCLUSIVE`。因此普通 stride 基本覆盖 CQE 读流，但未充分覆盖 SQE 写流。本轮新增可选 `stride_exclusive` 模型和相应 `HardPFExReq`，只在写流触发时请求写权限。完整重建 gem5 后，冷启动并保存 `shell-ready-modern-ddr5-stride-exclusive-20261014` checkpoint，官方 provider 首轮 9.045 Mops/s，紧接预热 16.742 Mops/s，后续同会话回归为 16.271 Mops/s。另一次重启的官方库预热及紧接回归为 16.671/16.502 Mops/s。正式模型的稳定值约 16.3–16.7 Mops/s，仅为实机 23.098 的 70–72%，尚未达到 ±20% 精度。

同一模型的 4KiB READ 为 46,666.84 MiB/s，实机 45,172.39 MiB/s，误差 +3.3%；128B `read_lat` 短样本均值 2.38µs，实机 3.38µs。128B latency 用了 1024 次，尾部百分位不可与 16384 次实机样本直接比较。大包吞吐接近实机，但小包带宽与固定时延还未校准完成。

### 官方库 128B READ 的 gem5 统计

在第二次 checkpoint 恢复后先跑两次官方库，再用 guest 伪指令仅对第三次发起端测试前后做 `resetstats/dumpstats`；该轮结果 16.529 Mops/s，原始输出在 `experiments/gem5-read-stride-exclusive-roi-stats-20261014/`。忙核 `switch_cpus2` 为 3,292,067 周期、6,182,940 条指令、IPC 1.878，约每次 READ 201 周期、377 条指令。L1D demand miss 为 79,991 次（4.88 次/READ），其中 55,931 次命中已有 MSHR、24,060 次形成新的 MSHR miss。预取器发出 217,372 次请求，`pfUseful=56,666`，`pfLate=159,718`；另外 41,865 个候选跨越 guest 4KiB 页边界没有发出。忙核 DTLB miss 6,401 次，提交内存屏障 19,907 次，rename serialize stall 884,690 周期。预取器显著减少了早期无预取配置的 miss，但仍有晚到和跨页损失。累计 stall 不能相加当作独占耗时。

曾考虑只在 gem5 DMB LD 解码中去掉写屏障标记；[Arm 对 DMB LD 的说明](https://developer.arm.com/community/arm-community-blogs/b/tools-software-ides-blog/posts/generate-litmus-tests-automatically-diy7-tool)指出它还须保证“先前 load → 后续 store”顺序，所以该修改已撤销。第一次重建时，SCons 复用了旧的 `decoder-ns.cc.inc`，使随后的屏障诊断、stride 距离 4、跨页预取和最初的优化 provider 实验都仍在错误的解码器上执行。**这些实验的数据无效，不参与结论或校准。**对应目录为 `gem5-read-exclusive-barrier-*`、`gem5-read-stride-exclusive-d4-20261014/`、`gem5-read-stride-exclusive-xpage-20261014/` 及最初的优化 provider 跑次。后来强制删除生成的 decoder 并重建，确认 DMB LD 同时保留读、写排序标记。构建脚本现检查这一条件，防止静默复用错误生成文件。

## 正确 DMB 解码器下的正式对照

正确重建后，官方 provider 的恢复首轮/预热为 9.066/16.964 Mops/s，见 `experiments/gem5-read-correct-dmb-stock-20261014/`。CQE 提前 4 项、SQE 提前 4 项且仅对 provider 两个热路径对象用 `-O3` 的优化版，另一次恢复中预热为 18.169/18.030 Mops/s；ROI stats 跑次为 18.628 Mops/s。`commit.membars` 仍约 1.99 万次。相比上文官方 provider 的 ROI 样本，忙核 L1D demand miss 从 79,991 降至 45,585，周期从 3,292,067 降至 2,924,148，说明包率缺口的主要可修复部分在发起端 provider 对 CQE/SQE 的缓存访问。上述优化版并非每轮都达到实机 23.098 Mops/s 的 80% 门槛（18.479 Mops/s）。

保持 CQE 提前 4 项、将 SQE 提前量改为 8 项后，同一恢复会话中两次预热为 18.886/18.998 Mops/s，随后一次为 18.298，另一次 ROI stats 跑次为 18.891 Mops/s；四次均值约 18.77 Mops/s（实机的 81.3%），但单次最低值为 79.2%，不能宣称稳定达到 80%。忙核 L1D demand miss 为 44,778，周期为 2,888,342，`commit.membars=19,894`。单独将 CQE 提前量增至 8（SQE=4）得到 17.865/17.820；CQE=8、SQE=8 得到 18.773/17.528。因此 SQE=8/CQE=4 是目前较优的参数，不是单调增加 lookahead。原始数据分别在 `experiments/gem5-read-correct-dmb-*`。

此参数的 4KiB READ 为 46,087.30 MiB/s，较实机 45,172.39 MiB/s 高 2.0%。128B latency 及完整包长趋势仍需单独校准。软件 `prfm` 是从公开 UMDK 源码构建出的优化 provider；它不会改变交换延迟、端口数、READ credit 或 ARM 屏障语义。现有数据只能说明这个组合可改善模拟客户端热路径，不能证明真实 Kunpeng 950 也需要相同的预取策略。

原始测量：`experiments/gem5-read-hotpath-*/`、`experiments/gem5-read-stride-*/`。实际首轮对照跑次的单次 host wall 约 40–50 秒；stride 4KiB/128B latency 两组分别 47.3/63.3 秒。网络/UDMA 原版八组设备计数与既有 [`gem5-read-provider-inner-profile-20261014.md`](gem5-read-provider-inner-profile-20261014.md) 一致：READ 窗口、SQ 深度、丢包、重传、线速 pacing 均未触顶。

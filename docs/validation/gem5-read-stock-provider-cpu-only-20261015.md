# 原版 UDMA provider + gem5 CPU 优化的 READ 对照（2026-10-15）

## 问题与对照条件

只保留 gem5 侧的 `stride_exclusive` L1D 预取器和 ArmO3 LQ64/SQ64；使用原版 UMDK `liburma-udma.so`，不使用 CQE4/SQE8 软件预取和两个文件的单独 `-O3` 重编。双端仍为单 Jetty、单 400 Gbit/s 端口、交换芯片每次 500 ns、端点到交换每段 375 ns，`-n 16384 -l 256 -Q 1 -R 8192 -T 8192 -C 524288`。现有 `urma_perftest` 的双端虚拟时间同步补丁在原版及优化版镜像中相同；此实验所说的“原版”特指 UDMA provider，不表示恢复未经改动的上游 benchmark。

对 `out/official-udma.cpio.gz` 与 `out/optimized-udma.cpio.gz` 解包逐文件比较，唯一不同的文件是 `lib/urma/liburma-udma.so`。前者库 SHA-256 为 `e0cd5c649e80e697dc1ee8aefa478459a0651d61a92603b81cf7307b312125f7`，后者为 `d8a20b0820e5322762a5590705489e2514c6842e8cd5d09a3b211ee55e2d6736`。两边 `urma_perftest` 与 `liburma.so.0` 哈希完全相同。两个校准 checkpoint 的 run manifest 除镜像路径、来源 checkpoint 名外相同，包括 LQ64/SQ64、`stride_exclusive`、网络/同步时延。新建原版 checkpoint `shell-ready-modern-ddr5-stock-lq64-sq64-link375-switch500-20261015`，已通过 `start-ready` 自动选择并实际恢复、双端同步和计时器检查。

## 实测结果

128B READ 恢复首跑 9.975 Mops/s，作为冷态预热样本保留但不计入稳态。紧接三次稳态为 17.359、16.831、16.971 Mops/s，均值 **17.054 Mops/s**，为实机 23.098 Mops/s 的 **73.8%**。优化版 provider 在相同 gem5 配置和链路参数下，之前三次为 19.321、18.174、19.319 Mops/s，均值 18.938（82.0%）；另一轮紧接着三次为 19.295、19.280、20.120，均值 19.565（84.7%）。这说明实验版 provider 的贡献约为 1.9–2.5 Mops/s；不同会话存在跑次波动，不能把这个差值当成精确的硬件因果效应。

原版 provider 预热后连续扫包长；带宽数值与实机同取 `urma_perftest` 的平均带宽字段。优化版列用 2026-10-15 相同网络配置的样本，其中 128B 是一次偏低的 sweep 跑次，另见上方重复结果。

| READ 包长 | 原版带宽 | 原版/实机 | 优化版带宽 | 优化版/实机 |
| ---: | ---: | ---: | ---: | ---: |
| 2B | 32.61 | 86.0% | 34.38 | 90.7% |
| 32B | 535.63 | 76.5% | 554.41 | 79.2% |
| 128B | 2077.76 | 73.7% | 2114.60 | 75.0% |
| 512B | 8052.42 | 71.7% | 9649.93 | 85.9% |
| 2048B | 32651.09 | 74.3% | 36785.42 | 83.7% |
| 4096B | 46662.79 | 103.3% | 46673.60 | 103.3% |
| 8192B | 47003.23 | 103.5% | 47008.72 | 103.5% |

**结论：** 只优化 gem5 CPU/缓存并使用原版 provider，无法使 128B READ 稳定达到实机包率的 80%；大包 4–8KiB 已与优化版一样达到约 47 GB/s，说明软件预取主要改变小包包率段。LQ64/SQ64 在原版 provider 下的独立增益未在这次实验中单独测定，不能从本表推断它本身贡献了多少。

## 证据

- 原版 checkpoint：`checkpoints/shell-ready-modern-ddr5-stock-lq64-sq64-link375-switch500-20261015/`。
- 原版首跑与三次稳态：`experiments/gem5-read-stock-cpu-only-128b-20261015/results.csv`。
- 原版包长曲线：`experiments/gem5-read-stock-cpu-only-size-20261015/results.csv`。
- 优化版对照：`experiments/gem5-read-bw-link375-switch500-20261015/results.csv`、`experiments/gem5-read-calibrated-128b-repeat-20261015/results.csv`、`experiments/gem5-read-calibrated-bandwidth-20261015/results.csv`、`experiments/gem5-read-calibrated-8k-20261015/results.csv`。

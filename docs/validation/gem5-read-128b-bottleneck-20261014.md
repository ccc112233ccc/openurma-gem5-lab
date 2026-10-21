# 单 Jetty 128B READ 包率瓶颈与修复（2026-10-14）

## 问题与约束

实机参考为 Kunpeng 950、400 Gbit/s 单端口、两节点经一颗交换芯片、单 Jetty 的 URMA CTP/RM READ：128B 为 23.098464 Mops/s，约 2819.64 MB/s；4KiB 为 45,172.39 MB/s。用户目标是模拟包率和趋势约有 80% 精度。实验保持 `-n 16384 -l 256 -Q 1 -R 8192 -T 8192 -C 524288`，交换芯片一次经过 500ns、1 个物理端口。gem5 从 shell-ready checkpoint 恢复，ROI 切至 ArmO3，两端五参与者 fence 校验通过。第一次恢复后的 READ 只有约 9–11 Mops/s，是明显的预热跑次，不用于稳态比较。

## 根因定位

先前无 L1D 预取器时，官方 UDMA provider 的预热包率约 9.9 Mops/s；加普通 stride 预取器约 13.7；让 SQE 写流预取请求写权限后约 16.3–17.0。正确 AArch64 DMB LD 解码器下，官方 provider 一次 ROI 16.529 Mops/s，发起端忙核 3,292,067 周期、79,991 次 L1D demand miss、339,077 次 LQFull、233,310 次 IQFull、47,824 次 SQFull。设备 `read_window_stalls=0`、`sq_depth_rejects=0`、网络 pacing/丢包/重传为 0。4KiB READ 已约 46.7 GB/s，说明小包时不是 400G 端口上限，也未观察到 UDMA READ credit 上限。

公开 UMDK provider 的逐 WQE 编码与逐 CQE 消费占 post/poll API 的主体，见 [provider profiling](gem5-read-provider-inner-profile-20261014.md)。对两个热路径对象用 `-O3`，在 CQE 访问前预取 4 项、在 SQE 写入前预取 8 项，构建独立 `liburma-udma.so`；不删除 DMB LD/DMB ST，不改驱动或交换模型。`tools/build_udma_optimized.py` 检查固定 UMDK commit 和 provider 源码无改动；`./lab build optimized-initramfs` 产出与原版分开的 `out/optimized-udma.cpio.gz`。双端镜像中的库 SHA-256 为 `d8a20b0820e5322762a5590705489e2514c6842e8cd5d09a3b211ee55e2d6736`。

优化 provider 后的 ROI 样本为 18.891 Mops/s，忙核 2,888,342 周期、44,778 次 L1D miss、117,013 次 LQFull、80,457 次 IQFull、61,186 次 SQFull，提交屏障仍为 19,894 次。相对官方库，约 40.4 万周期和 3.5 万 L1D miss 被消除。由于五次正式镜像预热样本为 18.737、18.623、18.954、17.924、18.086 Mops/s，均值 18.465（实机的 79.94%），仅有 provider 预取仍不能稳定达到 80%。

LQ32→64 单因素下，LQFull 归零，但 SQFull 升至 133,636，三次预热 17.754/18.505/18.743 Mops/s；所以只扩大 LQ 不足。保持 LQ64 再将 SQ32→64，SQFull 降至 7,736，ROI 忙核约 2,858,349 周期、IPC 2.134；三次预热 19.243/19.396/18.705，额外 stats 跑次 19.096 Mops/s，均超过 80% 门槛。4KiB READ 46,720.99 MiB/s，较实机约 +3.4%。IQ64→128 对照三次仅 17.510/17.997/17.792，故保留 IQ64。更多 SQE 预取尝试（SQE8 的 L1、SQE16 的 L3/L1）均出现明显轮间波动，没有足够证据优于当前 CQE4/SQE8。

为最终配置从优化镜像重新冷启动，保存 `shell-ready-modern-ddr5-optimized-lq64-sq64-20261014`。`start-ready --profile modern --roi-sync --l1d-prefetcher stride_exclusive --o3-lq-entries 64 --o3-sq-entries 64` 会自动选择它。正式恢复首跑 11.725 Mops/s，随后三跑 19.037/19.679/18.595 Mops/s，均值 19.104 Mops/s，为实机 23.098 的 82.7%。最低值也达到 80.5%。镜像 SHA 和 checkpoint metadata 匹配，双端 provider 二进制哈希均如上。

同一正式恢复继续抽测包长；表中“带宽”沿用 `urma_perftest` 表头的 MB/sec 数值，直接与用户提供的相同字段比较。

| READ 包长 | gem5 平均带宽 | 实机平均带宽 | gem5/实机 |
| ---: | ---: | ---: | ---: |
| 2B | 36.21 | 37.92 | 95.5% |
| 32B | 575.06 | 699.88 | 82.2% |
| 128B | 2307.10 | 2819.64 | 81.8% |
| 512B | 9508.20 | 11230.64 | 84.7% |
| 2048B | 37788.30 | 43935.38 | 86.0% |
| 4096B | 46722.35 | 45172.39 | 103.4% |

2–2048B 保持约 19 Mops/s 的包率平台，4KiB 转入约 46.7 GB/s 的链路平台。`experiments/gem5-read-final-lq64-sq64-size-20261014/results.csv` 保留上述六组及所有 UART 原文。曾追加 64KiB、16384 次操作，但模拟耗时过长，已停该组；旧的 64KiB、4096 次对照见 [大包多 Jetty 验证](gem5-large-jetty-bandwidth-20261010.md)，本轮没有把旧结果算作最终镜像的新测量。

**解释边界：** LQ/SQFull 是重命名阶段的资源阻塞事件计数，不能相加为独占耗时；联合扩容使事件下降且包率改善，支持这两个有限资源共同形成背压。现有 ArmO3 仍是通用代理，不等同于 Kunpeng 950；实机系统基础页大小与微结构队列规格尚未拿到，故 64/64 是经仿真瓶颈选择的主流服务器参数，不应声称是实机硬件规格。绑到同一个 guest CPU 后包率仍波动，CPU 迁移不是主要原因。

## 仍需校准的 READ 固定延迟

4/8 正式镜像下，128B `read_lat` 1024 次短样本均值 2.29µs，实机 16384 次均值 3.38µs；模拟低约 32%。曾尝试 16384 次 gem5 顺序延迟长样本，数分钟未完成，已停止，没有拿它报告尾部百分位。固定延迟差距和小包包率缺口是不同现象，不能用提高交换芯片 500ns 来掩盖：该数值已按用户给的单次经过延迟设置。需要再核查端点处理时延、实机基础页和实际测试的更多细节。

2026-10-15 补充：后续将端点到交换芯片的每段传播延迟从 100ns 校准为 375ns，保持交换芯片每次 500ns，128B 平均延迟达到 3.43µs。详情与其他包长、带宽及尾部局限见 [网络固定延迟校准](gem5-read-network-delay-calibration-20261015.md)。本节前一段描述的是校准前状态。

## 证据与复现

- 正确解码器下官方库：`experiments/gem5-read-correct-dmb-stock-20261014/`，ROI 基准 `experiments/gem5-read-stride-exclusive-roi-stats-20261014/`。
- 4/8 正式镜像：`experiments/gem5-read-correct-dmb-optimized-cqe4-sqe8-formal-20261014/`、`gem5-read-correct-dmb-optimized-cqe4-sqe8-repeat-20261014/`、`gem5-read-correct-dmb-optimized-cqe4-sqe8-large-20261014/`、`gem5-read-correct-dmb-optimized-cqe4-sqe8-lat1024-20261014/`。
- 队列容量：`experiments/gem5-read-lq64-cqe4-sqe8-20261014/`、`gem5-read-lq64-cqe4-sqe8-stats-20261014/`、`gem5-read-lq64-sq64-cqe4-sqe8-20261014/`、`gem5-read-lq64-sq64-cqe4-sqe8-stats-20261014/`、`gem5-read-lq64-sq64-4k-20261014/`、`gem5-read-lq64-sq64-iq128-cqe4-sqe8-20261014/`。
- 最终 checkpoint 正式恢复：`experiments/gem5-read-final-lq64-sq64-formal-20261014/`、`gem5-read-final-lq64-sq64-size-20261014/`。
- 客户机/网络/UDMA 计数和 UART 原文在各目录，逐组验证退出码、两端相同结果与 ROI fence。`tools/profile_gem5_read_jettys.py` 支持 `--pin-cpu` 供 CPU 亲和性对照。
- 一个早期 gem5 `DMB LD` 解码实验的生成文件曾被 SCons 复用，相关旧结果无效；本页全部数值均来自强制重建并确认 DMB LD 读写排序之后，详见 [预取诊断](gem5-read-prefetch-diagnosis-20261014.md)。

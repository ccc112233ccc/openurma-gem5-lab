# 128B READ 的 CPU 与内存路径检查（2026-10-15）

## 对照条件

- 双节点、单物理端口、400 Gbit/s；链路传播 375 ns/段，交换转发 500 ns/次。
- gem5 `modern`：4 个 3.2 GHz ArmO3 核、LQ/SQ 64/64、16 个 `DDR5_6400_4x8` 控制器。客户机使用原版 `out/official-udma.cpio.gz`，两组都不修改 provider。
- 每次测量 1 Jetty、128B READ、16384 次操作、有效深度 8192。首跑单列，比较后续三次。真实机器的参考包率为 23.098464 Mops/s。
- 对照原配置 `stride_exclusive`；实验组只换成 `stride_exclusive_xpage`，其余参数相同。分别从新建的 shell-ready checkpoint 恢复。

## 内存交织已配置

`scripts/run/run-dual.sh` 的 modern profile 使用 16 个控制器、128B 通道交织、`RoRaBaCoCh` DRAM 地址映射、`xor_low_bit=0`。这在 `run-manifest.txt` 和 gem5 `config.ini` 中均可核对。它不是未开启交织的单通道配置。128B 条带的通道选择使用物理地址第 7–10 位；映射顺序决定通道内的行、bank 与列的映射，不能把它等同于真实 Kunpeng 950 的未公开地址哈希。

原配置一次带统计触发的稳态测量见 `experiments/gem5-read-stock-lq64-sq64-cpu-dram-stats-20261015/01-read-s128-j1.node1.stats.txt`。统计窗口覆盖客户机测试命令及其建立/退出阶段，不能把全部计数当作纯数据循环。16 个 DRAM 控制器均有请求，读队列平均长度为 1.0–1.5，`numRdRetry` 和 `numWrRetry` 均为零；各控制器数据总线利用率约 3%。这些数据没有显示 DRAM 通道饱和或队列反压。改交织粒度、bank 映射或增加内存通道，目前缺少能让 128B 包率提高约 35% 的瓶颈证据。

## CPU 侧统计与跨页预取实验

原配置的活跃发起核执行约 330.8 万周期、IPC 1.87。`rename.serializeStallCycles=909190`，`rename.IQFullEvents=252456`，`rename.SQFullEvents=7692`；这些是可能重叠的周期/事件，不能直接相加。其 DTLB 有 2863806 次访问、7510 次 miss；L1D demand miss 为 68844 次。L1D 预取器发出 197972 个请求、`pfUseful=53467`、`pfLate=144556`，检测到 42986 个跨越客户机 4KiB 页边界的候选。原预取器不注册 MMU，跨页候选不会直接进入可翻译的预取队列。

跨页预取组的活跃核执行约 324.1 万周期、IPC 1.91；预取器发出 223525 个请求，`pfUseful=61030`，`pfLate=162548`。这证实新设置改变了实际预取行为。核心 DTLB miss 为 7672 次，接近原配置；因此跨页预取没有解决页表容量/粒度问题。真实机器使用 64KiB 基础页，当前客户机内核是 `CONFIG_ARM64_4K_PAGES=y`，而客户机 perftest 也没有物理测试命令的 `--page_size 65536` 选项。用跨页预取代替 64KiB 页并不等价。

| 设置 | 冷态首跑 (Mops/s) | 后三次 (Mops/s) | 稳态均值 | 真实机器比例 |
| --- | ---: | --- | ---: | ---: |
| 原配置 | 9.975 | 17.359 / 16.831 / 16.971 | 17.054 | 73.8% |
| 跨页预取 | 9.778 | 17.296 / 17.833 / 16.956 | 17.362 | 75.2% |

实验组均值高 1.8%，但两组三次样本的波动范围重叠；目前不足以认定这项设置稳定提升包率，更不足以达到真实机器 80% 门槛（18.479 Mops/s）。原始数据分别位于 `experiments/gem5-read-stock-cpu-only-128b-20261015/results.csv` 和 `experiments/gem5-read-stock-xpage-lq64-sq64-128b-20261015/results.csv`。实验组统计位于 `experiments/gem5-read-stock-xpage-lq64-sq64-stats-20261015/00-read-s128-j1.node1.stats.txt`。

## 下一步按证据排序

1. **页粒度和地址翻译**：已构建并启动独立的 64KiB 页内核、匹配 UB 模块和 guest image；[试验记录](gem5-read-64k-page-trial-20261015.md)显示首次 READ 可完成，但第二次远端 SVA 页表遍历失败，尚不能开展有效 A/B。应先定位缺失页表项及 IOPF/重新注册行为。保留当前 4KiB checkpoint 作为基线。只改模拟器 TLB 项数不是同一个实验。
2. **前端/乱序资源和屏障**：检查真实 Kunpeng 950 的队列、提交宽度和内存顺序特征，再测试对应的 ArmO3 参数。当前 3.2 GHz 已高于真实 2.3 GHz；直接继续提高频率或任意增大 IQ/ROB 可能得到数字吻合，但不能证明模型正确。已有的优化 provider 上 IQ 64→128 测试反而降低包率。
3. **一致性与 DMA 路径**：统计 L1D/IOCache snoop、所有权转换、CQE/SQE cache line 往返及每次 READ 的等待周期。原版与优化 provider 的唯一镜像差异是 `liburma-udma.so`，其稳态 128B 包率约 17.05 与 19.10 Mops/s；这约 12% 的差距不能靠改 DRAM 布局解释。
4. **NUMA/实际内存映射**：真实机器有 4 个 NUMA 域、24 根 DDR5-6400 DIMM，当前单客户机模型没有对应 NUMA 拓扑。只有确认发起核、UB 控制器与内存的真实亲和关系后，才能合理设置这个因素；它更可能影响跨域流量和大数据传输，对本轮低 DRAM 利用率的小包测试不应先当成主要修正量。

大包 READ 在此前同一单端口配置已接近 400 Gbit/s 链路上限，而 128B READ 的 17–19 Mops/s 对应约 2–2.4 GB/s 有效数据，说明两个工作区间的限制不同。当前结论是 CPU/缓存/翻译/提交路径值得继续查，但“配上交织即可达到真实包率”不符合已有统计；交织已经生效。

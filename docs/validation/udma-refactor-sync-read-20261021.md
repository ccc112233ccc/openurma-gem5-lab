# UDMA 模型拆分后的同步 READ 对照（2026-10-21）

## 范围与配置

本次复测两台主机、每端一个 UDMA 端口、一颗 ns-3-UB 交换芯片、每端 400 Gbit/s、端点到交换芯片每段 375 ns、交换处理每次 500 ns。负载为单向 CTP READ、一个 Jetty、`-T 8192 -R 8192 -C 524288 -Q 1 -p 0`；带宽用 `-I 0 -l 256`，延迟用 `-I 128`。QEMU 用单核 Cortex-A72/TCG icount 与 `synchronization=required`；gem5 用 3.2 GHz ArmO3、LQ/SQ 64/64 与 ROI 同步，并恢复 `shell-ready-modern-ddr5-stock-64k-lq64-sq64-link375-switch500-20261015`。两边均为 64 KiB 客体页。准确配置见[QEMU 清单](../../results/udma-refactor-sync-read-20261021/qemu-run-manifest.txt)、[gem5 清单](../../results/udma-refactor-sync-read-20261021/gem5-run-manifest.txt)和[同步拓扑](../../results/udma-refactor-sync-read-20261021/qemu-topology-sync.json)；实机来源为[带宽表](../../results/physical-read-bw-20261009.csv)与[延迟表](../../results/physical-read-lat-20261009.csv)。

每个带宽点 2B、128B、4KiB 使用 16,384 次操作；64KiB 使用 2,048 次、1MiB 使用 256 次。实机所有带宽点都是 16,384 次，因此大包只作为平台趋势对照。每个仿真延迟点请求 256 次，gem5 输出 250 个有效样本；实机为 16,384 次，不能据此比较尾部百分位。带宽单位沿用 `urma_perftest` 的 MiB/s，包率为 Mpps，延迟为 µs。

## 结果

| 包长 | 实机带宽 | gem5 带宽 | gem5/实机 | QEMU 带宽 | QEMU/实机 | 实机延迟 | gem5 延迟 | QEMU 延迟 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 2 B | 37.92 | 40.14 | 105.9% | 0.92 | 2.4% | 3.38 | 3.48 | 8.58 |
| 128 B | 2,819.64 | 2,624.17 | 93.1% | 58.58 | 2.1% | 3.38 | 3.59 | 8.54 |
| 4 KiB | 45,172.39 | 46,502.46 | 102.9% | 1,874.80 | 4.2% | 3.75 | 3.97 | 8.72 |
| 64 KiB | 45,588.18 | 47,054.73 | 103.2% | 29,491.02 | 64.7% | 5.08 | 5.14 | 9.97 |
| 1 MiB | 45,609.15 | 47,180.51 | 103.4% | 46,713.96 | 102.4% | 25.64 | 24.89 | 29.84 |

128 B 包率：实机 23.098、gem5 21.497、QEMU 0.480 Mpps。4 KiB 包率：实机 11.564、gem5 11.905、QEMU 0.480 Mpps。五个 gem5 点的平均带宽和平均延迟均在实机的 80%–120% 区间内。QEMU 只有 1 MiB 带宽进入该区间；小包受当前单核 TCG 侧约 0.48 Mops/s 的操作速率限制，不能作为 Kunpeng 950 的 CPU 性能预测。大包带宽逐渐靠近 400 Gbit/s 链路上限。

与拆分前 `../../results/read-compare-20261021/README.md` 的六点同步扫描相比，这五个公共包长的趋势一致：gem5 仍在约 21 Mpps 的小包平台与约 47,000 MiB/s 的大包平台，QEMU 仍在约 0.48 Mpps 的小包平台。两轮小包迭代数不同，不能把逐点差值当成严格的性能回归幅度。

两套扫描均无双端命令错误。gem5 的每个用例还通过五方同步栅栏检查。汇总 CSV 已随仓库保存：[QEMU 带宽](../../results/udma-refactor-sync-read-20261021/qemu-read-bw.csv)、[QEMU 延迟](../../results/udma-refactor-sync-read-20261021/qemu-read-lat.csv)、[gem5 带宽](../../results/udma-refactor-sync-read-20261021/gem5-read-bw.csv)、[gem5 延迟](../../results/udma-refactor-sync-read-20261021/gem5-read-lat.csv)。逐点 UART 和栅栏增量保留在本机 `experiments/udma-refactor-sync-perf-20261021/`，该目录不提交。QEMU 延迟/带宽命令累计墙钟 112.2/36.1 秒；gem5 为 387.7/475.7 秒，不包括客体启动和 `./lab sync`。运行期间宿主机可用内存约 6.9–7.8 GiB，高于 3.2 GiB 保护线；结束后全部仿真进程已关闭。

首次 QEMU JSON 启动时，启动器默认选到旧的 4 KiB、1024/1024/4096 配置，`read_lat` 在 JFC 深度检查处退出。随后显式指定 64 KiB 内核/initramfs 及 8192/8192/524288 上限，重新启动并完成上述全部测试。根据这一发现，已将 JSON 启动器、旧命令行启动器和工作台预检的 QEMU 默认值改为相同的 64 KiB 配置。又以无覆盖项的 JSON 运行短测试，清单显示正确的内核、镜像和队列上限，128B READ 延迟与带宽命令均成功；短测试数值不用于性能对照，完整输出仍在本机实验目录。

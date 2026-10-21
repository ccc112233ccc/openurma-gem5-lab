# Apple Silicon 原生 gem5 验证（2026-10-20）

## 结论

Apple Silicon macOS 宿主机已运行本项目的 ARM64 gem5 全系统双节点、独立 UDMA 和 ns-3-UB。原有 Linux gem5 checkpoint 可恢复，客体同步成功，官方 `urma_perftest` 的五项同步短测全部通过。后续实验可选择 `--runtime=native`；这次短测只验证执行链路，不建立长时间带宽曲线或实机 80% 精度结论。

## 构建与配置

- `gem5/build/ARM_MAC/gem5.opt`：Mach-O ARM64，gem5 24.0.0.1，带 UB-HOST 适配器；与 Linux 的 `gem5/build/ARM/gem5.opt` 分目录。
- Python 3.13.5、SCons 4.8.1、Apple Clang；`scripts/build/build_gem5_macos.sh` 在项目内保存 Python 构建依赖。
- macOS 编译兼容补丁是 `patches/source/gem5/0004-macos-clang-compatibility.patch`；外部 Simbricks C 源的格式警告只在 macOS 编译该源时降级。
- Linux 客体内核、官方 UMDK、initramfs、bootloader 沿用已有产物。macOS 没有 KVM，验证使用 `modern` 的 checkpoint、Atomic 快速阶段与 ArmO3 ROI。
- 拓扑：`configs/topology-gem5-two-heads.json`，两个 400 Gbit/s 端口，各端到交换 375 ns，交换每次 500 ns；一端口、一 Jetty。
- 恢复：`shell-ready-modern-ddr5-stock-lq64-sq64-link375-switch500-20261015`，两端 gem5 和 UDMA 状态均成功恢复。启动输出在 `experiments/native-macos-smoke/run-dual/`。

## 端到端结果

第一次五项短测的前四项通过，第五项在 node1 开始同步栅栏时遇到未完成的主机侧 DMA/MMIO 请求，原代码提前 `fatal`。已改为暂停客体、让设备事务继续排空，并在 COMMIT 时核对不再有未完成事务。重新构建和恢复同一 checkpoint 后，结果为：

| 测项 | 结果 | 宿主机墙钟时间 |
| --- | --- | ---: |
| 128B SEND latency | 通过，无校验错误 | 17.784 s |
| 128B WRITE latency | 通过，无校验错误 | 10.480 s |
| 128B READ latency | 通过，无校验错误 | 10.715 s |
| 128B SEND bandwidth | 通过，无校验错误 | 10.786 s |
| 4KiB READ bandwidth | 通过，无校验错误 | 11.288 s |

总墙钟 **61.062 s**，`completed_case_count=expected_case_count=5`，`virtual_time_synchronized=true`。原始命令、双端串口输出、五方同步栅栏日志和机器清单在 `experiments/native-macos-smoke/sync-smoke-retry/`；以 `report.json` 为验收记录。4KiB 带宽用 64 次操作，目的是短测，不适合与实机吞吐量比较。

## 复现

```bash
./lab --runtime=native build gem5
export UBSIM_DUAL_OUT="$PWD/experiments/my-native-run/run-dual"
./lab --runtime=native topology start configs/topology-gem5-two-heads.json
./lab --runtime=native sync
./lab --runtime=native rma-regression --sync-smoke
./lab --runtime=native stop
```

构建后的 Apple Silicon gem5 实验命令在宿主机运行。项目的 Docker 运行与构建入口已移除；后续 Linux 客体重建需要 ARM64 Linux 宿主机或 x86_64 Linux 交叉构建环境。64KiB READ 的完整带宽对照见 [后续验证](gem5-macos-64k-read-curve-20261020.md)。

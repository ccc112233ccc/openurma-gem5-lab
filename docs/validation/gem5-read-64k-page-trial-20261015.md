# 64KiB 客户机页大小的 gem5 READ 试验（2026-10-15）

## 隔离构建与启动

在容器的大小写敏感文件系统中复制已有 OLK 6.6 构建树，切换到 `CONFIG_ARM64_64K_PAGES=y`、`CONFIG_ARM64_PAGE_SHIFT=16`，重编内核和官方 UB/UMMU/UDMA 模块。产物独立保存在 `artifacts/kernel-64k-20261015/`，没有覆盖 `artifacts/kernel/`。将同一原版 UMDK provider、`urma_perftest` 与新模块打包为 `out/official-udma-64k-20261015.cpio.gz`；两个程序在新旧镜像 manifest 中的 SHA-256 分别相同。`official-udma/build_modules.sh` 和 `build_initramfs.sh` 增加 `UB_V2M_BRIDGE_DIR` 覆盖项，以便单独编译匹配的桥接模块，不覆盖原构建产物。

首次启动时，模拟 UMMU 的 CAP2 只声明 4KiB 页，官方 UDMA 驱动报 `UMMU: doesn't support sva!` 并拒绝探测设备。设备模型现同时声明 4KiB 和 64KiB granule，并按 TCT 的 TGS 字段选择 4KiB（4 级）或 64KiB（3 级）页表遍历；针对 64KiB 遍历添加定向单元测试。隔离的设备构建目录为 `artifacts/udma-device-sim-build-linux-aarch64-64k/`，12/12 项模型测试通过。该能力目前仍是实验性的；模拟器尚未实现 UMMU IOPF 缺页重试。

修改后，两个 gem5 全系统客户机均启动、加载官方 UDMA 驱动并出现 `/dev/uburma/udma0`。在双方运行 `grep -m1 KernelPageSize /proc/self/smaps` 均返回 `64 kB`。双端网络同步通过。保存并实际恢复了独立 checkpoint：`shell-ready-modern-ddr5-stock-64k-lq64-sq64-link375-switch500-20261015`。其他 CPU、16 通道 DDR5、1 个 400 Gbit/s 端口、375 ns 链路传播、500 ns 交换转发、LQ/SQ64/64、原版 provider 与 4KiB 对照组相同。

## READ 结果及未解决问题

128B READ、1 Jetty、16384 操作、深度 8192 的恢复后首跑得到 **11.605 Mops/s**；另一恢复的首跑为 **11.260 Mops/s**。这两个是冷态首跑，不能与 4KiB 组约 17.054 Mops/s 的稳态均值比较。第一次试验的第二次运行在接收端出现远端 READ 翻译失败；定向复现把操作数和深度降为 1024 后，首跑 **9.981 Mops/s**，第二次仍未完成。原始首跑结果分别在 `experiments/gem5-read-stock-64k-lq64-sq64-128b-20261015/results.csv` 和 `experiments/gem5-read-stock-64k-repeat-short-20261015/results.csv`。完整负载的第二次在 `udma-node0/udma.log` 曾记录 `remote READ translation failed`，但该易变运行目录随后被 checkpoint 恢复清理；短负载的决定性日志已另存。

临时诊断确认短负载第二次的失败发生在 **SVA 页表遍历**，而不是 MAPT 权限检查或已翻译物理地址的 DMA：接收端 token 4、目标 `0xfffff3950000`、64KiB granule，根表 `0xa4ee4000` 中索引 `0x3f` 的第一级描述符为 **0**。原文保存在 `experiments/gem5-read-stock-64k-pagewalk-levels-20261015/node1-pagewalk-failure.log`。该根表项为何为空尚未确定：可能是新进程目标页尚未建映射而需要 IOPF，也可能是 TCT 根表在资源重新注册后未正确更新。不能凭这一条日志断言是哪一个。

因此本轮**证明了 64KiB 内核可在当前 gem5 上启动、官方 UDMA 可探测、首次 READ 可完成**，但还**没有可靠的 64KiB 稳态带宽值**，不能据此声称达到实测的 80% 包率门槛。临时逐项日志已从设备模型源码移除；保留了隔离镜像、checkpoint、64KiB 能力和页表遍历实现，以及保存的失败证据。后续需要核对第二次运行 TCT 根表、目标 VMA/PTE 与 UMMU invalidation，确认是否需要实现 IOPF 后，再重复三次稳态测量。当前校准仍以已通过重复运行的 4KiB 结果为准。

另尝试在新恢复的首跑前后用 gem5 stats helper 采集 CPU/TLB 计数。4KiB 对照首跑的工作核心记录 2,875,175 次数据 TLB 查询、6,671 次未命中，文件为 `experiments/gem5-read-stock-lq64-sq64-cpu-dram-stats-20261015/00-read-s128-j1.node1.stats.txt`。64KiB 这轮在 READ 输出表头后超过 5 分钟仍未完成；先前不带 stats helper 的两次完整首跑分别只耗时 43.300 和 45.062 秒。因此中止该轮，**没有得到可比较的 64KiB TLB 计数**，不能声称大页已经降低 TLB 未命中。该未完成尝试的参数 manifest 留在 `experiments/gem5-read-stock-64k-cpu-dram-stats-20261015/`；实验进程已停止。

## 2026-10-19：找到并修复第二跑失败的模型错误

用独立诊断镜像临时给官方驱动加日志后，失败的第二跑在 node1 给出同一个进程的 SVA 绑定和 SQ 固定页记录：`pid=121 tid=4 pgd=...2081bc00 root=0xa081bc00`，SQ 地址 `0xfffff3950000`、长度 131072 字节，`pin_queue_addr` 返回 0。设备模型却将 TCT 根表地址读成 `0xa081b000`，对错误的根地址访问索引 `0x3f` 得到零描述符。原始证据位于 `experiments/gem5-read-64k-driver-probe-20261019-pass2/node1-driver-binding.log` 和 `node1-device-full.log`。诊断驱动仅在隔离镜像中使用；测试后已经恢复两套内核源码并重编原模块，删除了临时诊断镜像，仅保留日志。

官方 UMMU 驱动在 `cfg_table.h` 将 `TCT_ENT2_TTBA` 定义为地址位 **[51:4]**。64KiB 页、48 位用户 VA 的顶层 PGD 仅有 64 项、512 字节，Linux 从 `pgd_cache` 分配，因此物理根表地址允许落在 4KiB 页内。模型 `CheckMapt` 原来用 4KiB 掩码，错误清掉地址位 [11:4]。现在按 TCT 字段保留 [51:4]，并增加 `0xa081bc00` 根地址的回归测试。UMMU 命令队列还改为读取队列命令后才推进消费者指针，避免把未取到的同步命令提前确认；该改动单独不能解决本次根地址错误。

原始 64KiB checkpoint 恢复后，修复版模型的四次连续短测（128B READ、1024 操作、深度 1024）全部完成，包率为 9.996、16.965、13.592、19.884 Mops/s。随后保持单端口、1 Jetty，使用 **16384 操作、8192 深度**连续完成三次完整 128B READ：**21.305、21.933、21.407 Mops/s**，均值 **21.548 Mops/s**，达到实测 23.098 Mops/s 的 **93.3%**；三次平均带宽为 2600.72、2677.36、2613.14 MiB/s。对应证据目录为 `experiments/gem5-read-64k-tct-root-fix-20261019-pass{1..4}` 与 `experiments/gem5-read-64k-tct-root-fix-full-20261019-pass{1..3}`。修复版设备模型 12/12 项测试通过，完整运行的设备日志未再出现页表遍历或 SQ 预取失败。

这个结论只覆盖已重复验证的 128B READ 点。64KiB 下其他包长、WRITE、尾延迟和更长稳态尚未重新测量；模型仍未实现 EVENTQ/IOPF 缺页重试，不能由本次修复推断这些行为已完整。

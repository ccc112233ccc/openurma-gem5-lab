# QEMU 双端 64KiB 官方 URMA 功能验证（2026-10-19）

本次将修复 TCT 页表根掩码后的设备模型重编为 macOS ARM64 程序，启动两个 QEMU TCG 客户机。使用原版 UMDK/provider 与官方 UDMA 驱动的 64KiB 内核/镜像，单端口 400 Gbit/s、端点到交换芯片每段 375 ns、交换转发 500 ns，JFS/JFR/JFC 配额为 8192/8192/524288。`run-qemu-dual-64k-20261019/run-manifest.txt` 保存完整输入。双端 `KernelPageSize` 均为 `64 kB`，均有 `/dev/uburma/udma0`。

`./lab qemu-rma-regression` 的 full 套件 **12/12 通过**，包含 SEND/READ/WRITE 延迟与带宽操作、SQ 环绕、64KiB 和 1MiB 分片；套件执行 29.573 秒。原始结果在 `experiments/qemu-64k-model-fix-20261019/report.json`、`results.csv` 和逐项 UART 文件。官方 API 的逐字节 `urma_integrity` smoke **3/3 通过**（SEND/WRITE/READ 各 4096B，含完成记录与边界保护检查），执行 6.898 秒；结果在 `experiments/qemu-64k-integrity-smoke-20261019/report.json`。macOS 设备模型 12 项单元/进程测试通过；进程测试需在允许本地 Unix socket 的环境运行。

运行采用 `synchronization=off` 的 QEMU 功能模式。测试中的 perftest 带宽和延迟字段**不是**可与实机比较的虚拟时间性能测量。QEMU 当前配置为单核 Cortex-A72、1GiB 内存，也不代表目标 Kunpeng 950。此次验证证明 64KiB 官方驱动和修复后的模型可在 QEMU 双端完成数据通路；没有重新验证 QEMU 同步计时模式、极端并发或故障恢复。

`tools/run_qemu_integrity.py` 现在读取 `UBSIM_QEMU_DUAL_OUT`，因此可对隔离的运行目录执行同一逐字节检查。诊断日志中启动阶段对未映射的管理 IOVA `0xffffffff0000` 有两次空页表探测；本次所有被接受的工作队列操作都完成，未出现 SQ 预取或有效 token 的翻译失败。实验结束后已停止五个仿真进程。

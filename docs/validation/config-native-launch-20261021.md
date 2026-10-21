# JSON 原生启动路径验证（2026-10-21）

## 改动

`./lab topology start FILE.json` 现在校验 JSON 后，由 Python 解析 gem5 profile 和机器参数、匹配 checkpoint，并直接启动 gem5/QEMU、UDMA、ns-3 与控制网络进程。`./lab topology plan` 显示实际的 JSON 启动入口。界面的 checkpoint 预检使用相同的 Python 解析结果，不再调用 `run-dual.sh --print-config`。

原有 `./lab start`、`./lab start-ready`、`./lab start-qemu-dual` 等命令行入口仍保留给历史实验；`status`、`sync`、`stop` 和内存压力触发的停止操作仍沿用既有运维脚本。gem5 profile 的原有默认参数固化在 `ubsim_lab/gem5_profile_defaults.json`，并有解析结果对照测试。

## 验证

- gem5 双机：从 `node-template-modern-4gb-lq64-sq64-stride-20261021` 恢复，gem5、两台 UDMA、ns-3 和控制网络进程均运行；`./lab sync` 两端成功，身份、25.16 MHz 架构计时器和独立 OOB 地址检查通过。
- gem5 数据通路：`./lab latency-pair 0 1 16 128` 双端退出码均为 0，客户端 128B SEND 平均 6.27 µs。这是连通性短测，不是性能校准。
- QEMU 双机：`configs/topology-qemu-two-heads.json` 从 Python 路径启动后，QEMU、两台 UDMA 和 ns-3 进程均运行，两端均到 guest shell ready；测试后已停止。
- `tools.test_config_runtime`、`tools.test_topology`、`tools.test_experiments`、`tools.test_web`、`tools.test_lab_cli` 共 55 项回归测试通过。gem5 的三份示例拓扑解析清单与保留的 shell 启动器输出逐项一致。

本次只做双机实际启动验证；三机及以上沿用同一个解析与启动路径，但尚未在这次重构后重新执行全流程。

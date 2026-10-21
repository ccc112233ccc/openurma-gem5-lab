# 三节点 checkpoint 与宿主机内存保护（2026-10-20）

宿主机为 16 GiB Apple Silicon。先前的三节点 `modern` 冷启动使用每台 32 GB 建模内存；客体启动、UDMA 激活和同步成功，但 checkpoint 写入未完成。没有留存事故发生时的内存峰值，因此不能断定卡顿一定由内存不足导致。留下的 `m5.cpt` 只是写入过程中的文件，不能视作可恢复 checkpoint。

本轮新增两道保护：gem5/QEMU 启动前检查宿主机可用内存，并在保留线外为每个客体预留约 1.25 GiB；运行中每 2 秒采样，连续两次低于主机总内存的 20%（至少 2 GiB）则停止本轮仿真并写 `memory-guard.stopped`。三节点及以上 checkpoint 若建模内存总量大于宿主机容量四倍则拒绝。16 GiB 本机因此拒绝三台各 32 GB 的 checkpoint。

工作台生成三台及以上 gem5 主机时改为每台 4 GB 建模内存，仍保留 2 GB Linux 可见内存、3.2 GHz ArmO3、16 通道 DDR5-6400、400 Gbit/s 链路、375 ns 端点链路和 500 ns 交换处理。两节点校准配置仍使用 32 GB。4 GB 是多主机演示的资源预算，不能直接沿用两节点的性能校准结论。

验证过程：

1. 三节点 4 GB 冷启动、UDMA ACTIVE、架构计时器与 OOB 网络检查通过；checkpoint 23 秒写出，目录约 157 MiB。写入时观测到约 65% 宿主机可用内存，未触发保护线。
2. 首次恢复失败，原因是 gem5 把 `std::vector<bool>` 的 flash 锁定状态序列化成 `0/1`，而恢复解析器只接受 `true/false`。`tools/normalize_gem5_checkpoint.py` 修正了新 checkpoint，保存脚本也会在发布未来 checkpoint 前做相同转换；gem5 源码补丁 `0005-Serialize-vector-bool.patch` 修复后续重新构建的二进制。
3. 修正后通过工作台创建的三节点任务恢复，等价且可复现的配置现保存为 `configs/topology-gem5-three-heads.json`。`./lab sync` 验证三台客体的计时器和唯一 OOB 地址，随后 `./lab stop` 正常关闭。工作台兼容性检查选中 `shell-ready-modern-3node-4gb-20261020`。

当前 Mac gem5 二进制未因新源码补丁重新编译，保存脚本中的转换仍是必要的。验证结束后仿真进程已停止。

# gem5 节点模板 checkpoint

## 保存边界

`node-template-v1` 只保存**一台** gem5 客体完成基本文件系统挂载、但尚未分配主机身份和加载 UDMA 驱动时的状态。其 `template-cpt/` 不包含按节点编号保存的 UDMA 状态。恢复时，每个 gem5 实例读取同一份只读模板，再分别建立运行时内存、主机名、带外 IP/MAC、UDMA 进程、EID 和网络连接。这样，同一机器配置能扩展为 2–8 个独立节点。

客体通过 `ubsim_template=1` 启动参数在身份初始化前执行 checkpoint 伪指令。恢复后启动器用 `readfile` 把节点编号传给各实例；无效编号会进入诊断路径，不会默认为 node0。`./lab sync` 还会检查实际编号、主机名和 IP/MAC，避免出现重复身份。

## 与协调 checkpoint 的区别

| 类型 | 保存阶段 | 复用条件 | 用途 |
| --- | --- | --- | --- |
| 节点模板 | 分配身份、加载官方 UDMA 驱动之前 | CPU、缓存、内存、设备参数和客体镜像指纹匹配；节点数及新节点外部链路可以改变 | 多节点任务快速配置与拓扑试验 |
| 逐节点协调快照 | 各节点驱动及 UDMA 状态已初始化 | 必须具有对应的逐节点状态，且端点设置符合兼容检查 | 固定拓扑反复性能实验 |

网络进程在恢复时按当前拓扑重建，不存入模板。模板恢复仍需执行后续官方驱动初始化，因此通常比同配置的逐节点 shell-ready 快照慢。改动机器参数或客体镜像后，应新建匹配模板；旧 32 GB 结果不能自动迁移为新 4 GB 配置的性能结论。

## 创建和恢复

只有明确需要新模板时才冷启动一次，例如：

```sh
./lab start --nodes 2 --profile modern --mem-size 4GB \
  --o3-lq-entries 64 --o3-sq-entries 64 \
  --l1d-prefetcher stride_exclusive --roi-sync --make-template
./lab checkpoint-template my-modern-4gb-template
```

之后在相同机器和镜像条件下恢复：

```sh
./lab start-ready --nodes 3 --profile modern --mem-size 4GB \
  --o3-lq-entries 64 --o3-sq-entries 64 \
  --l1d-prefetcher stride_exclusive --roi-sync
./lab sync
./lab stop
```

当前工作区的 modern/4GB 默认 initramfs 为 `out/official-udma-64k-node-template-20261021.cpio.gz`。Apple Silicon 上若仅需重新打包保留的模板镜像，可用 `./lab build template-initramfs`；它不会覆盖同名已有文件，以免使 checkpoint 的镜像哈希失效。Linux 上从源码构建客体栈的要求见[官方软件栈集成](guest-driver-integration.md)。具体两/三节点运行数据已放入[历史验证记录](validation/node-template-checkpoint-history-20261021.md)。

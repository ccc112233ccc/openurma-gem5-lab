# 单份 gem5 基础 checkpoint 复用多节点

## 设计

节点数不是 gem5 客体机器配置的一部分。新的 `node-template-v1` checkpoint 只保存一台 gem5 客体在 PID 1 挂载基本文件系统之后、设置主机名/IP/MAC 和加载 UDMA 驱动之前的状态。它只包含 `template-cpt/`，没有按节点编号保存的 UDMA 状态。每个恢复实例独立加载这份只读基础状态，拥有自己的运行时内存、UDMA 进程、EID 和链路；ns-3-UB 网络也按本次拓扑重新创建。

客体启动参数 `ubsim_template=1` 使 PID 1 在身份初始化前执行 checkpoint 伪指令。恢复时，启动器给各 gem5 进程的 `readfile` 参数分别指向只含节点编号的文件。客体读到 0–7 后才设置主机名、OOB IPv4/MAC，加载官方 UDMA 驱动。无编号或非法编号会停止在诊断 shell，不会悄悄复用 node0 身份。原有 shell-ready 协调 checkpoint 仍按每节点保存 gem5/UDMA 状态，并要求相应节点状态存在。

`start-ready` 对模板忽略节点数和新节点的外部链路字段，但继续核对 CPU、缓存、内存、设备参数和客体镜像 SHA-256。`sync` 现在检查客体实际的节点编号、主机名、IP 和 MAC，防止复制一个已有身份的 shell-ready checkpoint 后错误地报告“地址唯一”。工作台新建 gem5 任务统一以每台 4GB 为默认值；改变主机数量保留原机器配置。旧 32GB 任务和 checkpoint 未转换成 4GB。

## 构建和使用

当前 Apple Silicon 工作区没有 ARM64 Linux 交叉编译器。保留的 64KiB 镜像与已有 ARM64 `ubsim-readfile` 辅助程序可由 `./lab build template-initramfs` 重新打包；该命令为避免使现有 checkpoint 的镜像哈希失效，会拒绝覆盖同名输出。Linux 上的 `scripts/build/build-interactive-initramfs.sh` 已改为直接从源码编译静态 `ubsim-readfile`；为模板镜像指定新的 `OUT` 路径。当前模板镜像为 `out/official-udma-64k-node-template-20261021.cpio.gz`，modern 4GB 配置会默认使用它，其他配置仍使用原镜像。

模板只需为同一机器配置冷启动一次：

```sh
./lab start --nodes 2 --profile modern --mem-size 4GB \
  --o3-lq-entries 64 --o3-sq-entries 64 \
  --l1d-prefetcher stride_exclusive --roi-sync --make-template
./lab checkpoint-template node-template-modern-4gb-lq64-sq64-stride-20261021
```

之后 2–8 台同规格主机都可用 `./lab start-ready`，更换 `--nodes` 即可；不需复制 checkpoint 目录或修改 UDMA 状态。示例：

```sh
./lab start-ready --nodes 3 --profile modern --mem-size 4GB \
  --o3-lq-entries 64 --o3-sq-entries 64 \
  --l1d-prefetcher stride_exclusive --roi-sync
./lab sync
./lab latency-pair 0 2 32 128
./lab stop
```

## 2026-10-21 验证

- 单份基础 checkpoint 的 2 节点和 3 节点恢复均通过：客体实际主机名为 node0/1/2，MAC 尾字节 01/02/03，`urma_admin show` 报告 ACTIVE EID `0100/0101/0102`，架构计时器及 OOB 同步均通过。基础模板生成耗时 147 秒，2 节点恢复到身份检查完成耗时约 83 秒，3 节点约 91 秒。
- 与工作台默认 LQ64/SQ64、`stride_exclusive` 匹配的模板生成耗时 159 秒；从同一份模板恢复 3 节点并同步，node2→node0 的官方 128B SEND 时延短测完成 32 个测量样本，客户端平均 5.69µs。这是连通性冒烟验证，不作为实机性能校准。
- 同一最终模板恢复 2 节点，通过新增的客体身份/IP/MAC 检查，并完成官方 `SEND/WRITE/READ` 128B 时延、128B SEND 带宽和 4KiB READ 带宽共 5/5 项同步短测；全部返回码为 0、`validation_errors=[]`，套件耗时 54.5 秒。结果保存在 `experiments/node-template-2node-smoke-20261021/`。
- 16GiB Mac 的内存保护在模板冷启动和三节点恢复过程中没有触发；日志观测可用内存约 10.9–11.0GiB。两次运行后均停止了 gem5、UDMA 和交换进程。

机器参数或客体镜像改变仍需匹配的新基础模板。4GB 多节点模板目前没有复测先前 32GB 双节点 READ 带宽曲线，不能把原有精度结论直接迁移到新默认内存配置。

基础模板位于驱动初始化前，因此每次恢复仍需在客体内完成官方驱动加载；本机约 83–91 秒才到身份检查完成，明显慢于已初始化的逐节点 shell-ready checkpoint。这个成本换来了节点数量与身份的自由度；固定拓扑的重复性能试验仍可从模板启动一次后，再保存逐节点 shell-ready checkpoint 作为快速缓存。

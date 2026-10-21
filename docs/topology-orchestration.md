# 声明式拓扑编排

## 配置与执行

`./lab topology` 读取 JSON 任务，先由 [`ubsim_lab/topology.py`](../ubsim_lab/topology.py)完整校验，再把解析结果交给 Python 启动器。未知字段、悬空端口和当前后端不支持的结构会报错，不会被静默简化。启动计划与原始 JSON 一起保存在运行目录，便于复现实验。

```sh
./lab topology validate configs/topology-gem5-two-heads.json
./lab topology plan configs/topology-gem5-two-heads.json
./lab topology start configs/topology-gem5-two-heads.json
```

任务包含 `nodes`（机头）、`devices`（每台一个 UDMA）、`switches`（零或一个 ns-3-UB 交换机）、`links`、`execution`，gem5 还可包含全局 `machine` 参数。节点数组顺序决定编号：首台是 node0，EID 为 `0x100`；其后依次递增。每台 UDMA 的端口数及显式 Jetty 上限需一致。

## 两种网络结构

| 结构 | JSON 表示 | 链路时延解释 |
| --- | --- | --- |
| 单交换机 | `switches` 有一个 `ns3ub` 交换机，每个设备端口各有一条至交换机的链路 | 每条 `links[].latency_ns` 是该端点到网络的单向传播时延；`switches[0].delay_ns` 对每次交换转发计一次 |
| 直连 | `switches: []`，两台设备之间一条链路 | 链路 `latency_ns` 是完整的单向传播时延，启动器拆到两端边界；不创建交换节点 |

直连目前仅支持两台主机、每台一个物理端口和相同链路速率。它不是把交换机处理时间设为零的交换拓扑。交换拓扑的交换端口编号固定为 `node_index * device_ports + device_port`；每个物理端口必须且只能连接一次。同一 UDMA 的全部端口目前必须使用相同的速率和端点时延。详细计时归属见[仿真同步与计时](synchronization.md)。

示例：`configs/topology-gem5-two-heads.json`、`configs/topology-gem5-three-heads.json`、`configs/topology-gem5-asymmetric-links.json`、`configs/topology-gem5-direct-link.json`；QEMU 有对应的 `two-heads`、`asymmetric-links`、`direct-link` 与 `heterogeneous` 示例。示例值只是特定任务参数，并非全局硬件常量。工作台自动创建的任务保存在本机 `configs/ui-tasks/`，不会作为项目示例提交。

## 当前约束

- gem5 和 QEMU 各支持 2–8 台主机，不能在同一任务中混用。
- gem5 节点使用同一 `profile` 和全局机器参数；`machine` 可设置内存容量、ArmO3 LQ/SQ 和 L1D 预取器。QEMU 节点可分别设置 CPU 类型、1–16 核与 512–8192 MiB 内存；这些选择不表示已校准的昆鹏硬件性能。
- 当前仅支持一个交换机或两节点直连；每台主机一个 UDMA；每台设备相同的端口数和 Jetty 上限；EID 按节点顺序生成。
- QEMU 声明式任务不支持 checkpoint 恢复，`execution.checkpoint` 必须为 `none`。gem5 可用 `auto` 选兼容快照或 `none` 冷启动。`execution.sync` 取 `required` 或 `off`，具体意义见[同步说明](synchronization.md)。
- 三节点及以上的客体控制握手通过独立的本地以太网中继连接，UB 业务数据仍只在 ns-3-UB 中传输。启动器会检查宿主机内存与本地端口。

## checkpoint 与拓扑变更

节点模板 checkpoint 位于身份分配和 UDMA 驱动初始化之前。同一机器配置与镜像指纹匹配时，可从一份模板恢复 2–8 个独立实例，并为各实例重新分配身份、UDMA 与网络。旧的逐节点协调 checkpoint 已包含客体与 UDMA 状态，必须满足各节点状态及端点设置的兼容检查；ns-3 网络本身按新拓扑创建，不保存在该快照中。交换与直连变更能否复用已有快照，以启动器的完整指纹校验结果为准，不应仅凭节点数判断。见[节点模板 checkpoint](node-template-checkpoint.md)。

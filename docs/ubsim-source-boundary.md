# 源码归属与版本边界

UBSim 是本项目及 `UBSIM_*` 配置命名空间的名称。运行路径不依赖早期原型的兼容仓库、SystemC 框架或替代 provider。

| 职责 | 源码位置 |
| --- | --- |
| 官方 OLK UB/UBUS/UBASE/UBCORE/UBURMA/UDMA 驱动 | 固定版本 `oe66/` |
| 官方 UMMU 内核与用户态支持 | 固定版本 `deps/ummu/` |
| `liburma`、`urma_admin`、`urma_perftest`、UDMA provider | 固定版本 `sources/umdk/` |
| gem5 主机、机器配置及薄适配器 | `gem5/`、`configs/arm64_fs.py`、`integrations/gem5/` |
| QEMU 薄适配器 | `integrations/qemu/` |
| 设备行为 | `components/udma-model/`、`components/udma-device-sim/` |
| CTP、交换、直连与链路 | `sources/ns-3-ub/`、`integrations/ns3ub/` |
| 进程间协议 | `protocol/ub_host/`、`protocol/ub_net/` |
| 可复现的上游变更 | `patches/source/` |

`scripts/fetch-sources.sh` 与根目录 [`SOURCE_REVISIONS.md`](../SOURCE_REVISIONS.md)给出固定版本的获取方式。官方代码与项目自有仿真模型分开维护；项目自有的 `ubsim_ub_v2m.ko` 是给模拟平台提供 MSI 域的桥接模块，不是官方驱动。

加载模块、工具能运行或一次 benchmark 成功，不代表设备所有行为均已覆盖。操作级覆盖与假设见[驱动覆盖审计](validation/official-driver-coverage.md)。历史记录仅作证据，不是构建输入。

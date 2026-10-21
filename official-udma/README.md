# 官方 UDMA 客体软件栈

本目录提供 OLK UB/UMMU/UDMA 内核栈的构建输入、镜像打包入口和硬件服务契约。官方内核模块与 UMDK 在 ARM64 客体里运行；gem5/QEMU 本身只编入 UB-HOST 平台适配器，硬件行为由独立 UDMA 设备和 ns-3-UB 网络模型提供。完整构建与加载链路见[官方软件栈集成](../docs/guest-driver-integration.md)。

## 构建入口

- [`kernel.fragment`](kernel.fragment) 与 [`build_modules.sh`](build_modules.sh)：合并内核配置，构建一致的内核与官方模块，并发布 `artifacts/kernel/`。
- [`build_initramfs.sh`](build_initramfs.sh)：调用通用打包器，将模块、官方 UMDK 库、provider、工具与客体启动脚本放进 initramfs。
- [`ub_v2m_bridge/`](ub_v2m_bridge/)：项目自有的模拟平台 MSI 域桥接；它与官方 UDMA 驱动源码分离。
- [`contract.tsv`](contract.tsv)：驱动访问到的设备服务清单。功能覆盖与限制见[驱动覆盖审计](../docs/validation/official-driver-coverage.md)。

标准用户入口是仓库根目录的 `./lab build kernel`、`./lab build umdk`、`./lab build initramfs`。ARM64 Linux 客体源码构建需要 Linux 或相应交叉构建环境；当前工作区已保留的 64 KiB 客体镜像有独立来源，不能假定 `./lab build all` 会重建它们。启动后在客体运行 `urma_admin show`，可检查 `udma0`、EID 和物理端口状态。数据路径还应通过两端 `urma_perftest` 或项目回归检查；模块加载成功本身不是数据路径验证。

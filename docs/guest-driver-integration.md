# 官方 UB 软件栈与客体镜像

## 集成边界

官方 OLK 内核模块和 UMDK 用户态程序运行在 **gem5/QEMU 的 ARM64 客体内**。gem5、QEMU 可执行文件只包含薄的 `UB-HOST` 适配器；它们不链接 `udma.ko`，也不把 provider 编译进模拟器。硬件行为由客体外的独立 UDMA 与 ns-3-UB 进程提供。运行路径为：

```text
客体 urma_perftest
  → liburma + 官方 liburma-udma.so + libummu.so.1
  → /dev/uburma/udma0、OLK UB/UMMU/UDMA 内核栈
  → gem5/QEMU 的设备 MMIO、DMA 与中断接口
  → UB-HOST 适配器 → 独立 UDMA 模型 → UB-NET → ns-3-UB
```

源码版本固定在 [`SOURCE_REVISIONS.md`](../SOURCE_REVISIONS.md)。上游构建输入位于 `oe66/`、`deps/ummu/` 与 `sources/umdk/`；工程自有更改放在 `patches/source/` 和集成目录。库存 provider 与实验优化 provider 必须作为不同镜像变体记录，不应把优化结果写作未修改官方 provider 的结果。

## 从源码到镜像

1. [`official-udma/build_modules.sh`](../official-udma/build_modules.sh)合并 [`kernel.fragment`](../official-udma/kernel.fragment)，检查 KSVA/MAPT 所需的最终 Kconfig，先构建内核 `Image`，再构建同一配置下的 UB、UMMU、UBCORE、UBURMA、UBASE 和 UDMA 模块，并发布到 `artifacts/kernel/`。
2. [`scripts/build/build_umdk.sh`](../scripts/build/build_umdk.sh)构建 ARM64 `liburma`、`liburma_common`、`urma_admin`、`urma_perftest`、官方 UDMA provider 和配套 `libummu.so.1`。
3. [`official-udma/build_initramfs.sh`](../official-udma/build_initramfs.sh)调用通用打包器，将模块、库、工具、启动脚本与依赖的动态库组装成 `out/official-udma.cpio.gz`。打包器核对内核/模块版本与必备 provider，避免把不一致的二进制拼在一起。
4. 客体的 [`overlay/init`](../overlay/init)挂载基本文件系统，检查库与 provider，按依赖顺序加载官方模块，建立设备与网络环境；`urma_admin show` 可用来检查 `udma0`、EID 和端口状态。带外以太网只用于测试工具的控制握手。

客体还装有项目自有的 `ubsim_ub_v2m.ko` 桥接模块，用 gem5 的 GICv2m 父中断域承接官方 UB 驱动需要的 MSI 域。这是模拟平台适配代码，不是官方 UDMA 驱动源码。`official-udma/contract.tsv` 与[驱动覆盖审计](validation/official-driver-coverage.md)记录驱动访问到的硬件服务及模型覆盖范围。

## 模拟器如何装载镜像

[`scripts/build/build_gem5.sh`](../scripts/build/build_gem5.sh)及 macOS 对应构建脚本把 [`integrations/gem5/ub_host_adapter/`](../integrations/gem5/ub_host_adapter/)以 gem5 `EXTRAS` 方式编进模拟器。[`configs/arm64_fs.py`](../configs/arm64_fs.py)实例化适配器并连接平台地址空间、DMA 和中断端口，同时把启动器传入的 `--kernel` 与 `--initrd` 提供给全系统客体。QEMU 使用其自己的 `UB-HOST` 适配器和客体启动镜像，但连接同一 UDMA 模型与网络进程。

当前工作区的 `modern` gem5 配置默认选择 64 KiB 基础页客体镜像：4 GB 节点模板使用 `out/official-udma-64k-node-template-20261021.cpio.gz`，其他内存配置使用 `out/official-udma-64k-20261015.cpio.gz`。QEMU 默认使用 `artifacts/kernel-64k-20261015/Image` 和 `out/official-udma-64k-20261015.cpio.gz`，并声明 JFS/JFR/JFC 上限为 8192/8192/524288。旧镜像仍可通过启动环境变量显式指定。这些 64 KiB 镜像是已保留的独立构建产物；`./lab build all` 尚不会从零重建它们。因此“完整重建成功”与“当前 64 KiB 性能任务可恢复”是两个不同检查项。

更改内核、provider、initramfs 或设备可见机器参数后，已有 checkpoint 可能因镜像哈希或机器指纹不匹配而不能复用。具体创建与恢复方式见[编译与运行](build-and-run.md)和[节点模板 checkpoint](node-template-checkpoint.md)。

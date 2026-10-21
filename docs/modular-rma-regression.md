# 模块化 RMA 回归

正式的启动方式、同步含义和产物位置见[编译与运行](build-and-run.md)。两节点 gem5/UDMA/ns-3-UB 的功能回归可从兼容 checkpoint 恢复后运行：

```sh
./lab start-ready --profile fast --provider official --no-sync
./lab sync
./lab rma-regression experiments/rma-regression
./lab stop
```

该功能门禁检查官方驱动和模型的 SEND、READ、WRITE 路径；`--no-sync` 输出不作为虚拟时间性能结论。具体用例、当时的运行时长和故障修复过程已归档在[历史回归记录](validation/modular-rma-regression-history.md)。运行选项由 `./lab --help` 的命令入口及当前回归程序解析。

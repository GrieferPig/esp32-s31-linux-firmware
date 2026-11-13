# 无线固件开发

本仓库独立维护固件源代码、SDK 配置和构建入口，不检出 Linux 消费端仓库。
直接子模块固定 Linux 接口头文件、OpenSBI 和 U-Boot；此处不构建 Linux 内核。

```sh
bash ci/checkout.sh
bash ci/setup.sh
bash ci/build.sh
```

输出归档为 dist/s31-firmware-inputs.tar。发布新版本后，消费端使用 Buildroot
验证输入并构建匹配的内核、模块、根文件系统与最终 radio.bin。
接口改变时须同步共享协议并按照消费端说明保留配置、清理 Linux 后重建。

详细内存预算、接口约束和测试说明见[英文开发指南](../en/radio-payload-development.md)。
构建与归档验证不能替代硬件测试。

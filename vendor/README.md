# 离线依赖

构建时对固定版本的 CUI 应用[滚动条补丁](../cmake/cui-scrollbar.cmake)，仅调整纵向滑块的绘制宽度。鼠标命中区使用上游尺寸。

`cui-7bb08ff7.tar.gz` 包含 CUI 1.2.0 的上游 C 源码、头文件、构建文件、内嵌字体数据与许可证。归档中的文件逐个与提交的 Git blob 哈希核对，构建时 CMake 再校验归档 SHA-256，可离线编译。[固定的上游提交](https://github.com/yappologistic/CUI/tree/7bb08ff7c56eb912fe7133985f8b7fa65098ea2e)

`pcre2-10.49.tgz` 是官方 10.49 发布包 `pcre2-10.49.tar.gz`，SHA-256 为 `929f0b20e62879252a15886b06c89f1edef61a363cbd5826fb041080a5e557ae`。程序静态链接 8 位 Unicode 引擎，许可证随 ZIP 分发。[PCRE2 发布页](https://github.com/PCRE2Project/pcre2/releases/tag/pcre2-10.49)

`updater-b81aac54.tar.gz` 取自 Hunlongyu/updater，SHA-256 为 `86b5a33eed47fafb2ff4e8419fc44ab406c05f7bae8a832446eb9856755808dc`。构建时补充处理 GitHub 将空发布源的 `/releases/latest` 重定向到 `/releases` 的情况。仅链接更新库与内置 cJSON，上游两份 MIT 许可证随发行包分发。[固定的上游提交](https://github.com/Hunlongyu/updater/tree/b81aac54fe750247320ef82afc1d0ee4b3ce3162)

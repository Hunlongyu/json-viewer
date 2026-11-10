<h1 align="center">JSON Viewer</h1>

<p align="center">
  Windows x64 上的只读 JSON / JSONL 查看器。<br>
  展开大型文档，搜索折叠节点里的键和值，单独导出需要的记录。
</p>

<p align="center">
  <a href="https://github.com/Hunlongyu/json-viewer/releases/latest/download/json-viewer-windows-x64.zip"><strong>下载便携 ZIP</strong></a>
  &nbsp; · &nbsp;
  <a href="https://github.com/Hunlongyu/json-viewer/releases/latest/download/json-viewer.exe">下载单文件 EXE</a>
  &nbsp; · &nbsp;
  <a href="https://github.com/Hunlongyu/json-viewer/releases/latest/download/checksums.txt">SHA-256 校验</a>
</p>

<p align="center">
  <a href="https://github.com/Hunlongyu/json-viewer/releases"><img src="https://img.shields.io/github/downloads/Hunlongyu/json-viewer/total?style=flat-square" alt="GitHub Releases 累计下载次数"></a>
</p>

<p align="center">
  <img src="docs/platformid-search.png" width="704" alt="在 30MB.json 中查找 PlatformID，树视图展开到第 1 个匹配项并高亮显示，共 11 处">
</p>

## 打开文件

运行 `json-viewer.exe`，将文件拖进窗口，或按 `Ctrl+O` 选择文件。若已把 JSON 复制到剪贴板，可通过 `File → Open Clipboard...` 直接查看。

- **JSON 文档**　单击节点左侧的加号，逐层展开对象或数组。普通 UTF-8 JSON 可以使用任意后缀。选择文件时可用 `All files` 显示 `.log`、`.config` 等文件。
- **JSON Lines**　`.jsonl` 和 `.ndjson` 按行读取。展开根节点即可逐条浏览记录，按 `End` 可跳到最后一行。

也可以从 PowerShell 传入文件路径。

```powershell
.\json-viewer.exe ".\events.jsonl"
```

## 查找与提取

按 `Ctrl+F` 输入关键词，再按 `Enter`。即使匹配项藏在折叠的节点中，查看器也会展开路径并高亮文本。用 `F3` 和 `Shift+F3` 在结果之间前后跳转。

| 要做什么 | 操作 |
| --- | --- |
| 只搜键名、值或原文 | 在 `Edit → Search Scope` 中选择 `Keys`、`Values` 或 `Document Text`。默认同时搜索键和值。 |
| 匹配大小写或使用正则 | 点击查找栏中的 `Aa` 或 `.*`。 |
| 复制当前节点 | `Ctrl+C` 复制值，`Ctrl+N` 复制名称，`Ctrl+P` 复制路径。 |
| 导出一条记录或子树 | 选中节点后按 `Ctrl+E`。右键菜单还可将对象或数组导出为紧凑或格式化 JSON。 |
| 导出整份文档 | 选择 `File → Export Data As`。JSONL 支持原样导出。 |

文件在其他程序中更新后，按 `F5` 重新读取。最近打开的 8 个文件可从 `File → Open Recent` 找到。按 `F1` 查看更多快捷键。

## 字体与便携设置

选择 `View → Change Font...` 打开 Windows 字体对话框。树文字默认使用 Segoe UI，大小为 13 个逻辑像素。对话框以磅显示字号，可选 6 至 36 磅。

**换一台电脑时，把 `json-viewer.exe` 和同目录的 `settings.ini` 一起复制，即可带走字体设置。** 程序目录不可写时，设置保存在 `%LOCALAPPDATA%\JSON Viewer\settings.ini`。首次在可写目录运行时会沿用当前用户已有的设置。

## 版本与更新

在 `Help → Check For Update` 中手动检查稳定版。如有更新，点击 `Download Update` 下载，再点击 `Restart and Install` 重启安装。[更新日志](CHANGELOG.md) · [历史版本](https://github.com/Hunlongyu/json-viewer/releases)

<details>
<summary><strong>从源码构建与测试</strong></summary>

在 Windows x64 上安装 Visual Studio C++ 构建工具、Windows SDK、CMake 3.24 或更高版本，以及 PowerShell 7。在仓库根目录运行打包命令。

```powershell
pwsh -File scripts/package-windows.ps1
```

可执行文件位于 `dist/windows-x64/json-viewer.exe`，便携包位于 `dist/json-viewer-windows-x64.zip`。运行读取器、视图、菜单和文档切换测试可使用以下命令。

```powershell
cmake --build target/c-win64 --config MinSizeRel --target reader-test view-test --parallel
ctest --test-dir target/c-win64 -C MinSizeRel --output-on-failure
pwsh -File scripts/test-node-menu.ps1
pwsh -File scripts/test-document-switch.ps1
```

</details>

# JSON Viewer

JSON Viewer 是一款 Windows x64 上的只读 JSON / JSONL 查看器。可以按树形结构浏览大型文件，在折叠的节点中搜索键或值，再复制或导出需要的部分。

![JSON Viewer 在 30MB.json 中搜索 PlatformID，自动展开到第 1 个命中，共 11 处](docs/json-viewer-search.png)

[![下载次数](https://img.shields.io/github/downloads/Hunlongyu/json-viewer/total?style=flat-square)](https://github.com/Hunlongyu/json-viewer/releases)

## 快速开始

1. 下载并解压 [Windows x64 便携包](https://github.com/Hunlongyu/json-viewer/releases/download/v0.2.2/json-viewer-windows-x64.zip)，双击 `json-viewer.exe` 即可启动。若从源码构建，可在 `dist/windows-x64/` 找到同名程序。
2. 将 `.json` 或 `.jsonl` 文件拖入窗口，也可以按 `Ctrl+O` 选择文件。要查看剪贴板中的 JSON，选择 `File → Open Clipboard...`。
3. 单击节点左侧的加号展开对象或数组。打开 JSONL 时，展开根节点即可按记录浏览。按 `Ctrl+C` 可复制选中项的值。

`.jsonl` 和 `.ndjson` 按逐行 JSON 读取。其他后缀或无后缀文件按普通 JSON 读取；例如保存为 `.txt`、`.log` 或 `.config` 的 UTF-8 JSON 文档，可直接拖入窗口，或在打开对话框中选择 `All files`。

也可以在程序目录中运行以下 PowerShell 命令打开文件。

```powershell
.\json-viewer.exe ".\example.json"
```

树文字默认使用 Segoe UI，字号为 13 个逻辑像素。选择 `View → Change Font...` 可直接打开 Windows 字体对话框，选择字体、字形和字号。对话框以磅为单位，可选 6 至 36 磅。

字体、字形和字号优先保存在 `json-viewer.exe` 同级的 `settings.ini`，复制这两个文件即可带走设置。程序目录不可写时使用 `%LOCALAPPDATA%\JSON Viewer\settings.ini`。首次在可写目录中运行时，会沿用当前用户已有的字体设置。可在 `[Tree]` 下编辑 `Family` 和以逻辑像素计的 `Size`，重新打开窗口后生效。

## 查找、复制与导出

| 操作 | 用法 |
| --- | --- |
| 查找键或值 | 按 `Ctrl+F` 输入关键词，再按 `Enter`。查看器会展开命中项的路径并高亮匹配文字。按 `F3` 跳到下一处，`Shift+F3` 回到上一处。 |
| 调整查找范围 | 在 `Edit → Search Scope` 中选择键、值或两者。选择 `Document Text` 可搜索原始 JSON 文本。查找栏的 `Aa` 切换大小写匹配，`.*` 切换正则表达式。 |
| 复制值或路径 | 选中节点后，按 `Ctrl+C` 复制值，按 `Ctrl+P` 复制路径。右键单击对象或数组可选择紧凑或格式化 JSON。 |
| 导出数据 | 按 `Ctrl+E` 导出选中项，或通过 `File → Export Data As` 导出整份文档。JSONL 可原样导出，也可选中一条有效记录并将其导出为格式化 JSON。 |
| 重新读取文件 | 文件在其他程序中修改后，按 `F5` 载入最新内容。最近打开的 8 个文件可从 `File → Open Recent` 访问。 |

更多快捷键可在程序中按 `F1` 查看。

## 版本与更新

在 `Help → Check For Update` 手动检查稳定版。发现新版本后，点击 `Download Update` 下载，再点击 `Restart and Install` 安装并重启。版本说明见 [GitHub Releases](https://github.com/Hunlongyu/json-viewer/releases)。

## 从源码构建

在 Windows 上安装 Visual Studio C++ 构建工具、Windows SDK、CMake 3.24 或更高版本，以及 PowerShell 7。在项目根目录运行：

```powershell
pwsh -File scripts/package-windows.ps1
```

脚本生成 `dist/windows-x64/json-viewer.exe` 和 `dist/json-viewer-windows-x64.zip`。ZIP 包含程序和第三方许可证。

# JSON Viewer

Windows x64 只读 JSON / JSONL 工具。正式程序使用 C11、Win32 和 CUI。主代码在 `native/`，固定依赖归档在 `vendor/`。构建会校验归档摘要。

## 构建与测试

在仓库根目录用 PowerShell 7 执行：

```powershell
pwsh -File scripts/package-windows.ps1
cmake --build target/c-win64 --config MinSizeRel --target reader-test view-test --parallel
ctest --test-dir target/c-win64 -C MinSizeRel --output-on-failure
pwsh -File scripts/test-node-menu.ps1
```

本机于 2026-11-10 使用 `C:\Program Files\CMake\bin\cmake.exe`（4.4.4）和 `C:\Program Files\Microsoft Visual Studio\18\Community` 的 x64 工具链验证。GitHub Actions 使用 Windows x64 托管运行器。

输出位于 `dist/windows-x64/`。本地大型样本在 `bin/`，该目录含基准程序和日志，保留在本机。缺少这些样本时 CTest 使用合成输入。

## 工程与发布

保持打开源文件的只读语义，按字节位置区分重复键。菜单命令、快捷键与 README 应同步。软件仅在用户发起检查更新时访问发布服务。

版本取自根 CMakeLists.txt。更新 CHANGELOG.md 后推送对应的 `vX.Y.Z` 标签，Actions 构建、测试并将 Windows x64 EXE、便携 ZIP 和校验文件发布到同一标签。

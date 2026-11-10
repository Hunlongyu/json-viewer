#Requires -Version 7.0
param([string]$Viewer='dist/windows-x64/json-viewer.exe',[switch]$Stress,[switch]$GiB)
$ErrorActionPreference='Stop'
if($Stress){
    $build=Join-Path (Split-Path $PSScriptRoot -Parent) 'target/c-win64'
    cmake --build $build --config MinSizeRel --target view-test
    if($LASTEXITCODE){throw 'C view stress test build failed'}
    ctest --test-dir $build -C MinSizeRel -R native-view --output-on-failure
    if($LASTEXITCODE){throw 'C view stress tests failed'}
}
& (Join-Path $PSScriptRoot 'test-c-windows.ps1') -Viewer $Viewer -GiB:$GiB

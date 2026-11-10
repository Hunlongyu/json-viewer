#Requires -Version 7.0
param([ValidateSet('Win32')][string]$Backend='Win32')
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
Push-Location $project
try {
    $build='target/c-win64'
    cmake -S . -B $build -A x64
    if($LASTEXITCODE){throw 'CMake configure failed'}
    cmake --build $build --config MinSizeRel --target json-viewer --parallel
    if($LASTEXITCODE){throw 'C build failed'}
    $out=[IO.Directory]::CreateDirectory((Join-Path $project 'dist/windows-x64'))
    $source="$build/_deps/cui-src"
    $notices=@('LICENSE','THIRD_PARTY.md','assets/fonts/LICENSE-Inter.txt','assets/fonts/LICENSE-Geist.txt','assets/fonts/LICENSE-Lucide.txt','third_party/LICENSE-Unicode.txt')
    $parts=foreach($file in $notices){"CUI / $file";Get-Content -LiteralPath (Join-Path $source $file) -Raw -Encoding utf8}
    $stb=Get-Content -LiteralPath (Join-Path $source 'third_party/stb_truetype.h') -Raw -Encoding utf8
    $parts+='stb_truetype and stb_rect_pack'; $parts+=$stb.Substring($stb.LastIndexOf('ALTERNATIVE A'))
    $parts+='PCRE2 10.49'; $parts+=Get-Content -LiteralPath "$build/_deps/pcre2-src/LICENCE.md" -Raw -Encoding utf8
    $parts+='winautoupdate'; $parts+=Get-Content -LiteralPath "$build/_deps/updater-src/LICENSE" -Raw -Encoding utf8
    $parts+='cJSON'; $parts+=Get-Content -LiteralPath "$build/_deps/updater-src/src/third_party/cJSON-LICENSE.txt" -Raw -Encoding utf8
    $notice=Join-Path $out 'THIRD-PARTY.txt'
    $parts -join [Environment]::NewLine | Set-Content -LiteralPath $notice -Encoding utf8
    $exe=Join-Path $out 'json-viewer.exe'
    $payload=(Get-Item "$build/MinSizeRel/json-viewer.exe").Length+(Get-Item $notice).Length
    if($payload -gt 8000000){throw "Windows release exceeds 8 MB ($payload bytes)"}
    try {
        Copy-Item -LiteralPath "$build/MinSizeRel/json-viewer.exe" -Destination $exe -Force -ErrorAction Stop
    } catch [IO.IOException] {
        # Windows permits renaming an in-use image even when overwriting its bytes fails.
        $prior=Join-Path $project ("target/previous-windows-"+[guid]::NewGuid().ToString('N')+".exe")
        $next=$prior+'.new'
        [IO.File]::Copy((Join-Path $project "$build/MinSizeRel/json-viewer.exe"),$next)
        try {
            [IO.File]::Move($exe,$prior)
            try {[IO.File]::Move($next,$exe)}catch{[IO.File]::Move($prior,$exe);throw}
        } finally {if([IO.File]::Exists($next)){[IO.File]::Delete($next)}}
    }
    $zip='dist/json-viewer-windows-x64.zip'
    Compress-Archive -LiteralPath @($exe,$notice) -DestinationPath $zip -CompressionLevel Optimal -Force
    $sum='dist/checksums.txt'
    @($exe,$zip) | ForEach-Object {
        "$((Get-FileHash -Algorithm SHA256 -LiteralPath $_).Hash.ToLowerInvariant())  $([IO.Path]::GetFileName($_))"
    } | Set-Content -LiteralPath $sum -Encoding ascii
    foreach($oldDll in @('vcruntime140.dll','vcruntime140_1.dll')){[IO.File]::Delete((Join-Path $out $oldDll))}
    Get-Item -LiteralPath @($exe,$notice,$zip,$sum) | Select-Object Name,Length
} finally {Pop-Location}

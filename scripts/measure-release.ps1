#Requires -Version 7.0
param([ValidateRange(1,30)][int]$Runs=5,[string]$Output='target/acceptance')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$outputDir=[IO.Directory]::CreateDirectory((Join-Path $root $Output))
$samples=@('23MB.json','30MB.json','sdk-events_s.jsonl','sdk-events_x.jsonl')
$backends=[ordered]@{
    Win32='dist/nsh-windows-x64/json-preview.exe'
    GPUI='dist/gpui-windows-x64/json-preview-gpui.exe'
    'GPUI Kit'='dist/gpui-kit-windows-x64/json-preview-gpui-kit.exe'
    Dadroit='bin/DadroitViewer.exe'
}
foreach($backend in $backends.Keys){
    $exe=$backends[$backend]
    $hash=(Get-FileHash (Join-Path $root $exe)).Hash
    foreach($sample in @('(empty)')+$samples){
        foreach($cache in @('warm','cold','cold-image')){
            if($sample -eq '(empty)' -and $cache -eq 'cold'){continue}
            if($cache -eq 'cold-image' -and $sample -notin @('(empty)','30MB.json')){continue}
            foreach($mode in @('navigation','search')){
                if($mode -eq 'search' -and ($backend -eq 'Dadroit' -or $cache -eq 'cold' -or $sample -notin @('30MB.json','sdk-events_s.jsonl','sdk-events_x.jsonl'))){continue}
                $id=$backend.Replace(' ','-')+'-'+$sample.Trim('(',')')+'-'+$cache+'-'+$mode
                $path=Join-Path $outputDir.FullName ($id+'.jsonl')
                if(Test-Path -LiteralPath $path){
                    $existing=@(Get-Content $path|ConvertFrom-Json)
                    $needsLogQueries=$sample -like '*.jsonl' -and ($mode -eq 'search' -or $backend -eq 'Dadroit')
                    if($existing.Count -eq $Runs -and !($existing|Where-Object sha256 -ne $hash) -and (!$needsLogQueries -or $existing[0].query_set -eq 'log-scalars')){continue}
                }
                Write-Output $id
                $rows=@(& "$PSScriptRoot/measure-windows.ps1" -App $backend -Viewer $exe -Samples $sample -Runs $Runs -PrivateDesktop -ColdData:($cache -ne 'warm' -and $sample -ne '(empty)') -ColdImage:($cache -eq 'cold-image') -Search:($mode -eq 'search'))
                if($rows.Count -ne $Runs){throw "Incomplete trial set: $id"}
                [IO.File]::WriteAllLines($path,[string[]]$rows)
            }
        }
    }
}

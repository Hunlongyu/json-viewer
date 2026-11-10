#Requires -Version 7.0
param([ValidateRange(1,30)][int]$Runs=5)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$exe=Join-Path $root 'target/release/preview-measure.exe'
$controlDir=[IO.Directory]::CreateDirectory((Join-Path $root 'target/bench-input'))
$fixture=Join-Path $controlDir.FullName ('escaped-'+[guid]::NewGuid().ToString('N')+'.jsonl')
try{
    [IO.File]::WriteAllText($fixture,('{"\u4e2d\u6587":"\u4e2d\u6587"}'+[Environment]::NewLine)*4096)
    foreach($file in @('23MB.json','30MB.json','sdk-events_x.jsonl','escaped.jsonl')){
        $path=if($file -eq 'escaped.jsonl'){$fixture}else{Join-Path (Join-Path $root 'bin') $file}
        $cases=switch($file){
            {$_ -in @('23MB.json','30MB.json')} {
                [pscustomobject]@{mode='verified-root-serde';q=$null}
                [pscustomobject]@{mode='verified-root-sonic';q=$null}
                [pscustomobject]@{mode='verified-root-incremental';q=$null}
            }
            'sdk-events_x.jsonl' {
                [pscustomobject]@{mode='search-raw';q='"local_index"';count=50232}
                [pscustomobject]@{mode='search-raw';q='"local_index": 50232';count=1}
                [pscustomobject]@{mode='search-raw';q='json-preview-absent-b03201b002c8';count=0}
            }
            'escaped.jsonl' {
                [pscustomobject]@{mode='search-keys';q='中文';count=4096}
                [pscustomobject]@{mode='search-values';q='中文';count=4096}
                [pscustomobject]@{mode='search-raw';q='中文';count=0}
            }
        }
        for($run=0;$run -le $Runs;$run++){
            for($i=0;$i -lt $cases.Count;$i++){
                $case=$cases[($run+$i)%$cases.Count]
                $argsForExe=@($case.mode,$path)
                if($null -ne $case.q){$argsForExe+=$case.q}
                $line=& $exe @argsForExe
                if($LASTEXITCODE){throw "Failed $file / $($case.mode)"}
                $data=$line|ConvertFrom-Json -AsHashtable
                if($null -ne $case.q -and $data.hits -ne $case.count){throw "Unexpected match count in $file"}
                if($run){$data.file=$file;$data.run=$run;$data.query=$case.q;$data|ConvertTo-Json -Compress}
            }
        }
    }
}finally{if([IO.File]::Exists($fixture)){[IO.File]::Delete($fixture)}}

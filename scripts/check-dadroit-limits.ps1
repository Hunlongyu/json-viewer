#Requires -Version 7.0
# Read the actual local viewer's response to the two large fixtures.
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
if(!('BenchmarkDesktop' -as [type])){Add-Type -Path (Join-Path $PSScriptRoot 'BenchmarkDesktop.cs')}
Add-Type -AssemblyName UIAutomationClient
foreach($sample in @('sdk-events_s.jsonl','sdk-events_x.jsonl')){
    $viewer=[BenchmarkDesktop]::new((Join-Path $project 'bin/DadroitViewer.exe'),(Join-Path (Join-Path $project 'bin') $sample))
    try {
        $null=$viewer.WaitWindow('Dadroit')
        $limit=[Diagnostics.Stopwatch]::StartNew()
        do{
            $names=foreach($hwnd in $viewer.VisibleWindows()){
                $w=[System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
                $w.Current.Name
                foreach($el in $w.FindAll('Descendants',[System.Windows.Automation.Condition]::TrueCondition)){
                    if($el.Current.Name){$el.Current.Name}
                }
            }
            if($names -match 'license.*size|file.*limit|file.*larg|larger|50\s*MB|maximum|exceeds'){break}
            Start-Sleep -Milliseconds 100
        }while($limit.Elapsed.TotalSeconds -lt 4)
        [pscustomobject]@{file=$sample;ui=@($names|Select-Object -Unique)} | ConvertTo-Json -Compress
    }finally{$viewer.Dispose()}
}

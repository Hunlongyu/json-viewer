#Requires -Version 7.0
param(
    [ValidateSet('Dadroit','Win32','GPUI','GPUI Kit')][string]$App = 'Win32',
    [string[]]$Samples = @('23MB.json','30MB.json'),
    [ValidateRange(1,30)][int]$Runs = 5,
    [string]$Viewer = 'target/release/json-preview.exe',
    [switch]$Search,
    [switch]$ColdData,
    [switch]$ColdImage,
    [switch]$PrivateDesktop,
    [switch]$Explorer
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (($ColdData -or $ColdImage) -and !('ColdSample' -as [type])){Add-Type -Path (Join-Path $PSScriptRoot 'ColdSample.cs')}
if($ColdImage -and !$PrivateDesktop -and $App -ne 'Dadroit'){throw 'Use a private trial desktop for cold executable images'}
if($Explorer -and ($ColdImage -or $PrivateDesktop)){throw 'Explorer runs in the interactive desktop'}
if($Explorer){
    $shell=New-Object -ComObject Shell.Application
    $folder=$shell.Windows()|Where-Object {$_.FullName -like '*\explorer.exe'}|Select-Object -First 1
    if(!$folder){throw 'An Explorer folder view is required for the Explorer handoff measurement'}
    $explorerApp=$folder.Document.Application
}
if (($PrivateDesktop -or $App -eq 'Dadroit') -and !('BenchmarkDesktop' -as [type])) {
    Add-Type -Path (Join-Path $PSScriptRoot 'BenchmarkDesktop.cs')
}
if (!('PreviewWindowInput' -as [type])) {
    Add-Type @'
using System;
using System.Diagnostics;
using System.Threading;
using System.Runtime.InteropServices;
public sealed class WindowProbe : IDisposable {
    [DllImport("user32.dll",SetLastError=true)]
    static extern IntPtr SendMessageTimeout(IntPtr hwnd,uint msg,IntPtr w,IntPtr l,uint flags,uint ms,out IntPtr result);
    readonly ManualResetEventSlim stop = new ManualResetEventSlim();
    readonly Thread worker;
    public double MaxRoundTripMs {get; private set;}
    public long MaxPrivateBytes {get; private set;}
    public int Samples {get; private set;}
    public int Timeouts {get; private set;}
    public WindowProbe(IntPtr hwnd, int pid) {
        worker = new Thread(() => {
            using (var p=Process.GetProcessById(pid)) {
                do {
                    var sw=Stopwatch.StartNew();
                    IntPtr unused;
                    if(SendMessageTimeout(hwnd,0,IntPtr.Zero,IntPtr.Zero,2,1000,out unused)==IntPtr.Zero) Timeouts++;
                    MaxRoundTripMs=Math.Max(MaxRoundTripMs,sw.Elapsed.TotalMilliseconds);
                    try { p.Refresh();MaxPrivateBytes=Math.Max(MaxPrivateBytes,p.PrivateMemorySize64); }
                    catch (InvalidOperationException) { break; }
                    Samples++;
                } while(!stop.Wait(5));
            }
        });
        worker.IsBackground=true;worker.Start();
    }
    public void Dispose(){stop.Set();worker.Join();stop.Dispose();}
}
public static class PreviewWindowInput {
    [StructLayout(LayoutKind.Sequential)] struct Point {public int X,Y;}
    [DllImport("user32.dll")] static extern bool ScreenToClient(IntPtr hwnd,ref Point point);
    public static void PressRow(IntPtr hwnd,int x,int y){
        var p=new Point{X=x,Y=y};
        if(!ScreenToClient(hwnd,ref p))throw new System.ComponentModel.Win32Exception();
        var point=(IntPtr)((p.Y<<16)|(p.X&0xffff));
        PostMessage(hwnd,0x200,IntPtr.Zero,point);
        PostMessage(hwnd,0x201,(IntPtr)1,point);
        PostMessage(hwnd,0x202,IntPtr.Zero,point);
    }
    delegate bool Visit(IntPtr h,IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(Visit cb,IntPtr data);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h,out uint id);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h,System.Text.StringBuilder s,int n);
    public static IntPtr NewHostWindow(int pid,int[] before,string title) {
        IntPtr found=IntPtr.Zero;
        EnumWindows((h,data)=>{
            GetWindowThreadProcessId(h,out uint owner);
            if(owner!=pid||Array.IndexOf(before,h.ToInt32())>=0)return true;
            var name=new System.Text.StringBuilder(512);
            GetWindowText(h,name,name.Capacity);
            if(!name.ToString().StartsWith(title,StringComparison.OrdinalIgnoreCase))return true;
            found=h;return false;
        },IntPtr.Zero);
        return found;
    }
    [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr Dispatch(IntPtr h,uint msg,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code,uint mapping);
    [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)]
    public static extern IntPtr SetText(IntPtr hwnd,uint msg,IntPtr w,string text);
    [DllImport("user32.dll")] public static extern IntPtr GetMenu(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr hwnd);
    [DllImport("user32.dll", SetLastError=true)]
    public static extern bool PostMessage(IntPtr hwnd, uint message, IntPtr key, IntPtr flags);
    [DllImport("oleacc.dll")] private static extern int AccessibleObjectFromWindow(
        IntPtr hwnd, uint id, ref Guid iid, [MarshalAs(UnmanagedType.IDispatch)] out object value);
    public static object AccessibleClient(IntPtr hwnd) {
        Guid id=new Guid("618736e0-3c3d-11cf-810c-00aa00389b71");
        object value;
        Marshal.ThrowExceptionForHR(AccessibleObjectFromWindow(hwnd,0xfffffffc,ref id,out value));
        return value;
    }
}
'@
}
$root = Split-Path $PSScriptRoot -Parent
$exe = (Resolve-Path (Join-Path $root $(if ($App -ne 'Dadroit') { $Viewer } else { 'bin/DadroitViewer.exe' }))).Path
$binaryHash=(Get-FileHash -LiteralPath $exe).Hash
$desktop = [System.Windows.Automation.AutomationElement]::RootElement
$any = [System.Windows.Automation.Condition]::TrueCondition

function Get-AppWindows([int[]]$OnlyPid) {
    $procs = if($OnlyPid){
        Get-Process -Id $OnlyPid -ErrorAction SilentlyContinue
    }else{Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($exe)) -ErrorAction SilentlyContinue | Where-Object Path -eq $exe}
    foreach ($p in $procs) {
        $pidFilter = [System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ProcessIdProperty,$p.Id)
        $desktop.FindAll('Children',$pidFilter)
    }
}
function Wait-Element ($window, [scriptblock]$matches) {
    $limit = [Diagnostics.Stopwatch]::StartNew()
    do {
        foreach ($element in $window.FindAll('Descendants',$any)) {
            if (& $matches $element.Current.Name) { return $element }
        }
        Start-Sleep -Milliseconds 5
    } while ($limit.Elapsed.TotalSeconds -lt 20)
    throw "Window did not reach the expected UI state: $($window.Current.Name)"
}
function Wait-Part($bar,[int]$part,[scriptblock]$accept) {
    $limit=[Diagnostics.Stopwatch]::StartNew()
    do {
        try {$text=$bar.accName($part)} catch {$text=''}
        if(&$accept $text){return $text}
        Start-Sleep -Milliseconds 2
    } while($limit.Elapsed.TotalSeconds -lt 12)
    throw "Expected status matching '$accept', last value '$text'"
}
function Send-Key([IntPtr]$target,[int]$vk) {
    $down=([long][PreviewWindowInput]::MapVirtualKey($vk,0) -shl 16) -bor 1
    $up=$down -bor [long]0xc0000000u
    if($App -eq 'Dadroit'){
        $null=[PreviewWindowInput]::Dispatch($target,0x100,$vk,$down)
        $null=[PreviewWindowInput]::Dispatch($target,0x101,$vk,$up)
    }else{
        $null=[PreviewWindowInput]::PostMessage($target,0x100,$vk,$down)
        $null=[PreviewWindowInput]::PostMessage($target,0x101,$vk,$up)
    }
}
function Set-Query($edit,$value,[IntPtr]$hwnd,[string]$query){
    if($App -eq 'Win32'){$value.SetValue($query);return}
    # GPUI Fast 0.1.9 exposes an edit value to UIA but rejects its SetValue action.
    # Exercise the actual field and its ordinary text-input path instead.
    $w=[System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
    $condition=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::Edit)
    $edit=$w.FindFirst('Descendants',$condition)
    $edit.SetFocus()
    $wait=[Diagnostics.Stopwatch]::StartNew()
    do{
        $edit=$w.FindFirst('Descendants',$condition)
        if($edit.Current.HasKeyboardFocus){break}
        Start-Sleep -Milliseconds 10
    }while($wait.Elapsed.TotalSeconds -lt 2)
    # The input handler is installed during the next GPUI frame after focus.
    Start-Sleep -Milliseconds 60
    $old=$edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
    Send-Key $hwnd 0x23
    for($i=0;$i -lt $old.Length;$i++){Send-Key $hwnd 8}
    if($old){Start-Sleep -Milliseconds 120}
    foreach($ch in $query.ToCharArray()){$null=[PreviewWindowInput]::Dispatch($hwnd,0x102,[int]$ch,1)}
    $limit=[Diagnostics.Stopwatch]::StartNew()
    do{
        $field=$w.FindFirst('Descendants',$condition)
        $observed=$field.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
        if($observed -eq $query){return}
        Start-Sleep -Milliseconds 10
    }while($limit.Elapsed.TotalSeconds -lt 3)
    throw "Text entry mismatch: expected '$query', observed '$observed'"
}
function Measure-Scroll($window) {
    $first=Wait-Element $window {param($name)$name -match '▸\s+#1\s+行'}
    $rect=$first.Current.BoundingRectangle
    $x=[int]($rect.Left+[math]::Min(120,$rect.Width/2))
    $y=[int]($rect.Top+$rect.Height/2)
    $point=($y -shl 16) -bor ($x -band 65535)
    $hwnd=$window.Current.NativeWindowHandle
    $action=[Diagnostics.Stopwatch]::StartNew()
    $probe=[WindowProbe]::new($hwnd,$window.Current.ProcessId)
    try {
        for($i=0;$i -lt 32;$i++){
            $null=[PreviewWindowInput]::PostMessage($hwnd,0x20a,[IntPtr](-120 -shl 16),$point)
            Start-Sleep -Milliseconds 8
        }
        $item=Wait-Element $window {param($name)$name -match '#(\d+)\s+行' -and [int]$Matches[1] -gt 45}
        $scrollMs=$action.Elapsed.TotalMilliseconds
    } finally { $probe.Dispose() }
    return [ordered]@{wheel_events=32;elapsed_ms=$scrollMs;observed_row=$item.Current.Name;
        max_roundtrip_ms=$probe.MaxRoundTripMs;probe_samples=$probe.Samples;timeouts=$probe.Timeouts}
}
function Measure-DadroitTree($window,$sample,$startupClock) {
    $lines=$sample -like '*.jsonl'
    $lastLine=if($sample -eq 'sdk-events_s.jsonl'){24324}else{50231}
    $items=$window.FindAll('Descendants',$any)
    $bars=@($items|Where-Object {$_.Current.ClassName -eq 'msctls_statusbar32'}|Sort-Object {$_.Current.BoundingRectangle.Top})
    $selected=[PreviewWindowInput]::AccessibleClient($bars[0].Current.NativeWindowHandle)
    $timing=[PreviewWindowInput]::AccessibleClient($bars[-1].Current.NativeWindowHandle)
    $tree=($items|Where-Object {$_.Current.ClassName -eq 'Window'}|Sort-Object {$_.Current.BoundingRectangle.Height} -Descending|Select-Object -First 1).Current.NativeWindowHandle
    $field=($items|Where-Object {$_.Current.ClassName -eq 'Edit'}|Sort-Object {$_.Current.BoundingRectangle.Width} -Descending|Select-Object -First 1).Current.NativeWindowHandle
    $next=($items|Where-Object {$_.Current.ClassName -eq 'Button' -and $_.Current.Name -eq '>'}|Select-Object -First 1).Current.NativeWindowHandle
    $action=[Diagnostics.Stopwatch]::StartNew()
    Send-Key $tree 0x24
    $null=Wait-Part $selected 1 {param($s)if($lines){$s -eq '$[0]'}else{$s -eq '$.Id'}}
    $firstMs=$action.Elapsed.TotalMilliseconds
    $firstFieldMs=$startupClock.Elapsed.TotalMilliseconds
    if($sample -eq '30MB.json'){
        Send-Key $tree 0x28
        $null=Wait-Part $selected 1 {param($s)$s -eq '$.ProfileSchemaVersion'}
    }
    if(!$lines){
        Send-Key $tree 0x28
        $null=Wait-Part $selected 1 {param($s)$s -eq '$.Runtime'}
    }
    $action.Restart()
    Send-Key $tree 0x27
    Send-Key $tree 0x28
    $null=Wait-Part $selected 1 {param($s)if($lines){$s.StartsWith('$[0].')}else{$s -eq '$.Runtime.Types'}}
    $typesMs=$action.Elapsed.TotalMilliseconds
    $data=[ordered]@{select_first_ms=$firstMs;types_ms=$typesMs;first_field_ms=$firstFieldMs}
    if($lines){
        Send-Key $tree 0x24
        $null=Wait-Part $selected 1 {param($s)$s -eq '$[0]'}
        $data['index_summary']=$timing.accName(2)
        if($data.index_summary -notmatch ('Lines:\s*'+('{0:N0}' -f ($lastLine+1)))){throw 'Dadroit did not index the expected record count'}
    }
    foreach($case in @('first','next','tail','none')){
        $term=switch($case){
            'first'{if($lines){'local_index'}else{'PlatformID'}}
            'tail'{if(!$lines){'WSManStackVersion'}elseif($lastLine -eq 24324){'1793953507517'}else{'1793958291119'}}
            'none'{'json-preview-absent-b03201b002c8'}
        }
        if($term){$null=[PreviewWindowInput]::SetText($field,0x000c,0,'');$null=Wait-Part $timing 3 {param($s) !($s -match 'Find:')}}
        if($case -eq 'next'){Start-Sleep -Milliseconds 100}
        $action.Restart()
        if($case -eq 'next'){
            $null=[PreviewWindowInput]::PostMessage($next,0x201,1,655365)
            $null=[PreviewWindowInput]::PostMessage($next,0x202,0,655365)
            $path=Wait-Part $selected 1 {param($s)if($lines){$s -and $s.StartsWith('$[1].')}else{$s -like '*.OperatingSystem.Instance.Properties.Platform.Type'}}
        }else{
            $null=[PreviewWindowInput]::SetText($field,0x000c,0,$term)
            if($case -eq 'none'){
                $null=Wait-Part $timing 3 {param($s)$s -match 'Find:'}
                $path=''
            }elseif($case -eq 'tail'){
                $path=Wait-Part $selected 1 {param($s)if($lines){$s -and $s.StartsWith(('$['+$lastLine+'].'))}else{$s -eq '$.Platform.PowerShell.WSManStackVersion'}}
            }else{
                $path=Wait-Part $selected 1 {param($s)if($lines){$s -and $s.StartsWith('$[0].')}else{$s -like '*.OperatingSystem.Instance.Constructors[[]0][[]0]'}}
            }
        }
        $data[$case+'_ms']=[math]::Round($action.Elapsed.TotalMilliseconds,2)
        $data[$case+'_path']=$path
    }
    return $data
}
foreach ($sample in $Samples) {
    $empty = $sample -eq '(empty)'
    $path = if (!$empty) { (Resolve-Path (Join-Path (Join-Path $root 'bin') $sample)).Path }
    for ($run = 0; $run -le $Runs; $run++) {
        $inputPath = $path
        $copy = $null
        $titleName = $sample
        $trialExe=$exe
        $imageDir=$null
        $imageFiles=@()
        if ($ColdData -and !$empty) {
            # The single-instance host can focus an existing document when a path is already open.
            $titleName = "json-preview-$([guid]::NewGuid().ToString('N'))-$sample"
            $copyDir = [IO.Directory]::CreateDirectory((Join-Path $root 'target/bench-input'))
            $copy = Join-Path $copyDir.FullName $titleName
            [ColdSample]::Copy($path,$copy)
            $inputPath = $copy
        }
        if($ColdImage){
            $imageDir=[IO.Directory]::CreateDirectory((Join-Path $root "target/bench-input/image-$([guid]::NewGuid().ToString('N'))"))
            $payload=@(Get-Item -LiteralPath $exe)+@(Get-ChildItem -LiteralPath (Split-Path $exe) -Filter '*.dll' -File)
            foreach($part in $payload){
                $dest=Join-Path $imageDir.FullName $part.Name
                [ColdSample]::Copy($part.FullName,$dest)
                $imageFiles+=$dest
            }
            $trialExe=Join-Path $imageDir.FullName ([IO.Path]::GetFileName($exe))
        }
        $hostPids=if($Explorer -and $App -eq 'Dadroit'){@(Get-Process -Name DadroitViewer -ErrorAction SilentlyContinue|Where-Object Path -eq $exe|ForEach-Object Id)}
        $before=if($Explorer -or (!$PrivateDesktop -and $App -ne 'Dadroit')){@(Get-AppWindows $hostPids | ForEach-Object {$_.Current.NativeWindowHandle})}
        $info = [Diagnostics.ProcessStartInfo]::new($trialExe)
        $info.UseShellExecute = $false
        if (!$empty) {
            if($App -eq 'Dadroit') { $info.Arguments = '"' + $inputPath + '"' }
            else { $info.ArgumentList.Add($inputPath) }
        }
        $window = $null
        $probe = $null
        $isolated = $null
        $sw = [Diagnostics.Stopwatch]::StartNew()
        if($Explorer){
            $process=$null
            $null=$explorerApp.ShellExecute($exe,('"'+$inputPath+'"'),(Split-Path $exe),'open',1)
        }elseif($PrivateDesktop -or $App -eq 'Dadroit'){
            $isolated=[BenchmarkDesktop]::new($trialExe,$inputPath)
            $sw=$isolated.Clock
            $process=$isolated.Process
        }else{$process = [Diagnostics.Process]::Start($info)}
        Write-Verbose "Trial $run, executable $exe, sample $inputPath, arguments $($info.Arguments), PID $($process.Id)"
        try {
            if($isolated){
                $hint=if($App -ne 'Dadroit'){'JSON 预览'}elseif($empty){'Dadroit JSON Viewer'}else{$titleName}
                $window=[System.Windows.Automation.AutomationElement]::FromHandle($isolated.WaitWindow($hint))
            }elseif($Explorer -and $App -eq 'Dadroit' -and $hostPids){
                do {
                    $owned=[PreviewWindowInput]::NewHostWindow(@($hostPids)[0],[int[]]@($before),"$titleName - Dadroit")
                    if($owned -ne [IntPtr]::Zero){$window=[System.Windows.Automation.AutomationElement]::FromHandle($owned);break}
                    Start-Sleep -Milliseconds 5
                }while($sw.Elapsed.TotalSeconds -lt 12)
            }else{do {
                $fresh = @(Get-AppWindows $hostPids | Where-Object {
                    $_.Current.NativeWindowHandle -notin $before -and
                    $(if($Explorer -and $App -ne 'Dadroit'){$_.Current.Name -eq 'JSON 预览'}
                      elseif($App -ne 'Dadroit') {$_.Current.ProcessId -eq $process.Id}
                      elseif($empty) {$_.Current.Name -like '*Dadroit*'}
                      else {$_.Current.Name.StartsWith("$titleName - Dadroit")})
                })
                if ($fresh.Count -eq 1) {$window=$fresh[0];break}
                Start-Sleep -Milliseconds 5
            } while ($sw.Elapsed.TotalSeconds -lt 20)}
            if (!$window) {
                $seen = @(Get-AppWindows | ForEach-Object {$_.Current.Name})
                throw "Expected new $App window for '$titleName'. Observed: $($seen -join ', ')"
            }
            if(!$process){$process=Get-Process -Id $window.Current.ProcessId}
            $windowMs = $sw.Elapsed.TotalMilliseconds
            $probe=[WindowProbe]::new($window.Current.NativeWindowHandle,$window.Current.ProcessId)
            $engine = $null
            $childrenMs = $null
            $indexMs = $null
            $indexMemory=$null
            $firstFieldMs=$null
            $firstSelectedMs=$null
            $rootMs=$null
            $typesMs=$null
            $scroll=$null
            $searchData = $null
            if ($empty) {
                if ($App -eq 'Dadroit') {
                    if(![PreviewWindowInput]::GetMenu($window.Current.NativeWindowHandle)){throw 'File menu unavailable'}
                    $fields=@($window.FindAll('Descendants',$any) | Where-Object {$_.Current.ClassName -eq 'Edit'})
                    if(!$fields.Count){throw 'Empty viewer has not created its controls'}
                }else{$null=Wait-Element $window {param($name)$name -eq '打开文件'}}
            } elseif ($App -eq 'Dadroit') {
                $limit=[Diagnostics.Stopwatch]::StartNew()
                do {
                    foreach($child in $window.FindAll('Descendants',$any)){
                        if ($child.Current.ClassName -ne 'msctls_statusbar32') {continue}
                        $status=[PreviewWindowInput]::AccessibleClient($child.Current.NativeWindowHandle)
                        for($i=1;$i -le $status.accChildCount;$i++){
                            try { $part=$status.accName($i) } catch { continue }
                            if($part -match '^Load:\s*[\d.,]+\s*ms'){$engine=$part.Trim();break}
                        }
                        if($engine){break}
                    }
                    if(!$engine){Start-Sleep -Milliseconds 5}
                } while (!$engine -and $limit.Elapsed.TotalSeconds -lt 20)
                if(!$engine){throw "Missing Dadroit load readout in $sample"}
            } else {
                $null=Wait-Element $window {param($name) $name -eq '就绪'}
            }
            $readyMs=$sw.Elapsed.TotalMilliseconds
            $snapshot=Get-Process -Id $window.Current.ProcessId
            $readyPrivate=$snapshot.PrivateMemorySize64
            $readyWorking=$snapshot.WorkingSet64
            if($App -eq 'Dadroit' -and !$empty){
                $searchData=Measure-DadroitTree $window $sample $sw
                $firstSelectedMs=$searchData.first_field_ms
            }elseif ($Search) {
                if ($sample -notin @('sdk-events_s.jsonl','sdk-events_x.jsonl','23MB.json','30MB.json')){throw "Missing search setup for $sample"}
                $jsonLines=$sample -like '*.jsonl'
                $editCondition=[System.Windows.Automation.PropertyCondition]::new(
                    [System.Windows.Automation.AutomationElement]::ControlTypeProperty,
                    [System.Windows.Automation.ControlType]::Edit)
                $edit=$window.FindFirst('Descendants',$editCondition)
                $inputValue=$edit.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern)
                $hwnd=[IntPtr]$window.Current.NativeWindowHandle
                $searchData=[ordered]@{}
                foreach($case in @('first','next','tail','none')){
                    $q=switch ($case) {
                        'first' {if($jsonLines){'local_index'}else{'PlatformID'}}
                        'tail' {if(!$jsonLines){'WSManStackVersion'}elseif($sample -eq 'sdk-events_x.jsonl'){'1793958291119'}else{'1793953507517'}}
                        'none' {'json-preview-absent-b03201b002c8'}
                    }
                    if ($q) {
                        Set-Query $edit $inputValue $hwnd $q
                        $null=Wait-Element $window { param($name) $name -eq '查找' }
                    }
                    $action=[Diagnostics.Stopwatch]::StartNew()
                    if(![PreviewWindowInput]::PostMessage($hwnd,0x0100,0x72,0x003d0001)){throw 'F3 delivery failed'}
                    $null=[PreviewWindowInput]::PostMessage($hwnd,0x0101,0x72,[IntPtr]([long]0xc03d0001u))
                    if($case -eq 'none') {
                        $null=Wait-Element $window { param($name) $name -eq '未找到匹配' }
                    } else {
                        $expected=if($case -eq 'next'){2}else{1}
                        $null=Wait-Element $window { param($name) $name -match "^第\s*$expected\s*处匹配" }
                    }
                    $searchData[$case + '_ms']=[math]::Round($action.Elapsed.TotalMilliseconds,2)
                    if($case -eq 'tail'){
                        $null=Wait-Element $window { param($name) if($jsonLines){$name -match "ts_ms\s+$q"}else{$name -match 'WSManStackVersion\s+\S+'} }
                        $searchData['tail_jump_ms']=[math]::Round($action.Elapsed.TotalMilliseconds,2)
                    }
                }
            } elseif (!$empty -and $App -ne 'Dadroit') {
                $rootRow=Wait-Element $window {param($name) $name -match '▸\s+(\$|JSONL)'}
                $rootBounds=$rootRow.Current.BoundingRectangle
                $action=[Diagnostics.Stopwatch]::StartNew()
                $hwnd=[IntPtr]$window.Current.NativeWindowHandle
                # Right arrow expands the selected root through the viewer's ordinary key handler.
                if($Explorer){
                    [PreviewWindowInput]::PressRow($hwnd,[int]($rootBounds.Left+20),[int]($rootBounds.Top+$rootBounds.Height/2))
                }else{
                    if(![PreviewWindowInput]::PostMessage($hwnd,0x0100,0x27,0x014d0001)){throw 'Key delivery failed'}
                    $null=[PreviewWindowInput]::PostMessage($hwnd,0x0101,0x27,[IntPtr]([long]0xc14d0001u))
                }
                $firstRow=Wait-Element $window {param($name) $name -match '^\s*(Id\s|[▸▾]\s+#1\s)'}
                $childrenMs=$action.Elapsed.TotalMilliseconds
                $firstFieldMs=$sw.Elapsed.TotalMilliseconds
                if($Explorer -and $sample -like '*.json'){
                    $idValue=$firstRow.Current.Name -replace '^\s*Id\s+',''
                    Send-Key $hwnd 0x24
                    Send-Key $hwnd 0x28
                    $null=Wait-Element $window {param($name)$name.StartsWith('字节 ') -and $name.EndsWith($idValue)}
                    $firstSelectedMs=$sw.Elapsed.TotalMilliseconds
                    Send-Key $hwnd 0x24
                }
                if($sample -match '^sdk-events_([sx])\.jsonl$'){
                    $records=if($Matches[1] -eq 's'){24325}else{50232}
                    $null=Wait-Element $window {param($name) $name -match "JSONL\s+$records\s+条记录"}
                    $indexMs=$action.Elapsed.TotalMilliseconds
                    $indexedProcess=Get-Process -Id $window.Current.ProcessId
                    $indexMemory=@{working_set_bytes=$indexedProcess.WorkingSet64;private_bytes=$indexedProcess.PrivateMemorySize64}
                    $scroll=Measure-Scroll $window
                }else{
                    $null=Wait-Element $window {param($name)$name -match '▸\s+Platform\s+\{'}
                    $rootMs=$action.Elapsed.TotalMilliseconds
                    Send-Key $hwnd 0x28
                    if($sample -eq '30MB.json'){Send-Key $hwnd 0x28}
                    Send-Key $hwnd 0x28
                    $expand=[Diagnostics.Stopwatch]::StartNew()
                    Send-Key $hwnd 0x27
                    $null=Wait-Element $window {param($name)$name -match '▸\s+Types\s+\{'}
                    $typesMs=$expand.Elapsed.TotalMilliseconds
                }
            }
            $live=Get-Process -Id $window.Current.ProcessId
            $probe.Dispose()
            $response=[ordered]@{samples=$probe.Samples;max_roundtrip_ms=$probe.MaxRoundTripMs;timeouts=$probe.Timeouts;max_private_bytes=$probe.MaxPrivateBytes}
            $probe=$null
            if($run) {
                [pscustomobject]@{
                    app=$App;sample=$sample;run=$run;cold_data=[bool]$ColdData;window_ms=[math]::Round($windowMs,2)
                    sha256=$binaryHash;workload=$(if($Search){'search'}else{'navigation'})
                    query_set=$(if($sample -like '*.jsonl' -and ($Search -or $App -eq 'Dadroit')){'log-scalars'})
                    ready_private_bytes=$readyPrivate;ready_working_set_bytes=$readyWorking
                    private_desktop=[bool]$isolated;cold_image=[bool]$ColdImage;explorer=[bool]$Explorer
                    first_field_ms=$firstFieldMs;first_selected_ms=$firstSelectedMs;all_root_ms=$rootMs;types_ms=$typesMs
                    ready_ms=[math]::Round($readyMs,2);engine_load=$engine;children_ms=$childrenMs
                    working_set_bytes=$live.WorkingSet64; private_bytes=$live.PrivateMemorySize64
                    peak_working_set_bytes=$live.PeakWorkingSet64;search=$searchData;index_ms=$indexMs;index_memory=$indexMemory;response=$response;scroll=$scroll
                } | ConvertTo-Json -Compress
            }
        } finally {
            if($probe){$probe.Dispose()}
            # Close only the new window from this iteration; Dadroit can reuse a running process.
            if($isolated){$isolated.Dispose()}else{
                if($window){
                    $owned=$window.Current.NativeWindowHandle
                    $null=[PreviewWindowInput]::PostMessage($owned,0x10,0,0)
                    if($Explorer -and [PreviewWindowInput].GetMethod('IsWindow')){
                        $closing=[Diagnostics.Stopwatch]::StartNew()
                        while([PreviewWindowInput]::IsWindow($owned) -and $closing.Elapsed.TotalSeconds -lt 2){Start-Sleep -Milliseconds 15}
                    }
                }
                if($process){$process.Dispose()}
                if($Explorer -and $App -eq 'Dadroit'){Start-Sleep -Milliseconds 1250}
            }
            if($copy){Remove-Item -LiteralPath $copy}
            foreach($part in $imageFiles){[IO.File]::Delete($part)}
            if($imageDir){[IO.Directory]::Delete($imageDir.FullName)}
        }
    }
}

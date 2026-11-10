#Requires -Version 7.0
# Exercise a dedicated instance. Each open below goes through its normal Windows picker.
param([string]$Viewer='dist/windows-x64/json-viewer.exe',[switch]$Stress,[switch]$GiB)
$ErrorActionPreference='Stop'
if(($Viewer.Replace('\','/')) -match '(^|/)(dist/windows-x64|target/c-win64)/'){
    if($Stress){
        $build=Join-Path (Split-Path $PSScriptRoot -Parent) 'target/c-win64'
        cmake --build $build --config MinSizeRel --target view-test
        if($LASTEXITCODE){throw 'C view stress test build failed'}
        ctest --test-dir $build -C MinSizeRel -R native-view --output-on-failure
        if($LASTEXITCODE){throw 'C view stress tests failed'}
    }
    & (Join-Path $PSScriptRoot 'test-c-windows.ps1') -Viewer $Viewer -GiB:$GiB
    return
}
Add-Type -AssemblyName UIAutomationClient
Add-Type -AssemblyName UIAutomationTypes
if (!('PreviewSmoke' -as [type])) {
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class PreviewSmoke {
    [StructLayout(LayoutKind.Sequential)] struct Pt {public int X,Y;}
    [DllImport("user32.dll")] static extern bool ScreenToClient(IntPtr h,ref Pt p);
    public static void Click(IntPtr hwnd,int x,int y){
        var p=new Pt{X=x,Y=y};
        if(!ScreenToClient(hwnd,ref p))throw new System.ComponentModel.Win32Exception();
        var point=(IntPtr)((p.Y<<16)|(p.X&65535));
        PostMessage(hwnd,0x200,IntPtr.Zero,point);
        PostMessage(hwnd,0x201,(IntPtr)1,point);
        PostMessage(hwnd,0x202,IntPtr.Zero,point);
    }
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
    [DllImport("user32.dll")] public static extern IntPtr GetLastActivePopup(IntPtr h);
    [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)] public static extern IntPtr SetText(IntPtr h,uint msg,IntPtr w,string text);
}
'@
}
$project=Split-Path $PSScriptRoot -Parent
$exe=(Resolve-Path (Join-Path $project $Viewer)).Path
$temp=[IO.Directory]::CreateTempSubdirectory('json-preview-smoke-').FullName
$log=Join-Path $temp '含空格 日志.ndjson'
$bad=Join-Path $temp '截断.json'
$utf8=Join-Path $temp '编码.json'
$locked=Join-Path $temp '独占.json'
$denied=Join-Path $temp '无权读取.json'
$wide=Join-Path $temp '扫描中.json'
$huge=Join-Path $temp '宽数组.json'
$any=[System.Windows.Automation.Condition]::TrueCondition
$editFilter=[System.Windows.Automation.PropertyCondition]::new([System.Windows.Automation.AutomationElement]::ControlTypeProperty,[System.Windows.Automation.ControlType]::Edit)
function Find-Visible([string]$pattern){
    $limit=[Diagnostics.Stopwatch]::StartNew()
    do{
        $names=@()
        foreach($e in $script:window.FindAll('Descendants',$any)){
            $name=$e.Current.Name
            if($name -match $pattern){return $e}
            $names+=$name
        }
        Start-Sleep -Milliseconds 15
    }while($limit.Elapsed.TotalSeconds -lt 8)
    throw "Missing '$pattern'. Visible names: $($names -join ', ')"
}
function Press-Key([int]$vk){
    $null=[PreviewSmoke]::PostMessage($script:hwnd,0x100,$vk,1)
    $null=[PreviewSmoke]::PostMessage($script:hwnd,0x101,$vk,[IntPtr]([long]0xc0000001u))
}
function Press-Element([string]$pattern) {
    (Find-Visible $pattern).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
}
function Click-Row([string]$pattern) {
    $bounds=(Find-Visible $pattern).Current.BoundingRectangle
    [PreviewSmoke]::Click($hwnd,[int]($bounds.Left+35),[int]($bounds.Top+$bounds.Height/2))
}
function Start-Term([string]$q) {
    $field=$window.FindFirst('Descendants',$editFilter)
    $field.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($q)
    $null=Find-Visible '^查找$'
    Press-Key 0x72
}
function Open-Sample([string]$path){
    Press-Element '^打开文件$'
    $limit=[Diagnostics.Stopwatch]::StartNew()
    do{
        $popup=[PreviewSmoke]::GetLastActivePopup($script:hwnd)
        $dialog=if($popup -and $popup -ne $script:hwnd){[System.Windows.Automation.AutomationElement]::FromHandle($popup) | Where-Object {$_.Current.ProcessId -eq $process.Id -and $_.Current.ClassName -eq '#32770'}}
        if ($dialog){break}
        Start-Sleep -Milliseconds 20
    }while($limit.Elapsed.TotalSeconds -lt 6)
    if(!$dialog){throw 'The native file picker did not open'}
    $dialogHandle=[IntPtr]$dialog.Current.NativeWindowHandle
    $limit.Restart()
    do {
        $edit=$dialog.FindAll('Descendants',$any) | Where-Object {$_.Current.ClassName -eq 'Edit' -and $_.Current.AutomationId -in @('1148','1001')} | Select-Object -First 1
        if($edit){break}
        Start-Sleep -Milliseconds 20
    } while($limit.Elapsed.TotalSeconds -lt 5)
    if(!$edit){throw 'No filename input in picker'}
    if(![PreviewSmoke]::SetText($edit.Current.NativeWindowHandle,0x000c,0,$path)){throw 'Failed to set picker filename'}
    $confirm=[PreviewSmoke]::GetDlgItem($dialogHandle,1)
    if(!$confirm -or ![PreviewSmoke]::PostMessage($dialogHandle,0x0111,1,$confirm)){throw 'Failed to confirm file selection'}
}
try {
    $lines=@('','{broken}','{"field":"\u4e2d\u6587"}','{"field":"last"}')
    [IO.File]::WriteAllText($log,($lines -join [Environment]::NewLine),[Text.UTF8Encoding]::new($true))
    [IO.File]::WriteAllText($bad,'{"ok":true,"unfinished":[1,')
    [IO.File]::WriteAllBytes($utf8,[byte[]](0x22,0xc0,0xaf,0x22))
    [IO.File]::WriteAllText($locked,'{}')
    $hold=[IO.File]::Open($locked,'Open','ReadWrite','None')
    $start=[Diagnostics.ProcessStartInfo]::new($exe)
    $start.UseShellExecute=$false
    $start.ArgumentList.Add((Join-Path $project 'bin/30MB.json'))
    $process=[Diagnostics.Process]::Start($start)
    $null=$process.WaitForInputIdle(5000)
    $limit=[Diagnostics.Stopwatch]::StartNew()
    do {$process.Refresh();$script:hwnd=$process.MainWindowHandle;if($hwnd){break};Start-Sleep -Milliseconds 10}while($limit.Elapsed.TotalSeconds -lt 5)
    $script:window=[System.Windows.Automation.AutomationElement]::FromHandle($hwnd)
    $null=Find-Visible '^就绪$'
    Press-Key 0x27
    $null=Find-Visible 'Runtime\s+\{'
    $null=Find-Visible 'Platform\s+\{'
    Press-Element 'Runtime\s+\{'
    $null=Find-Visible 'Types\s+\{'
    Write-Output 'PASS startup argument, ordered root and Runtime/Types'
    Open-Sample $log
    $null=Find-Visible ([regex]::Escape($log))
    Press-Key 0x27
    $null=Find-Visible 'JSONL\s+3\s+条记录'
    Press-Element '#1\s+行 2'
    $null=Find-Visible '行 2.*字节'
    $null=Find-Visible '行 2.*\{broken\}'
    Press-Element '#3\s+行 4'
    $null=Find-Visible 'field\s+last'
    Write-Output 'PASS native picker, BOM/CRLF, invalid row context and later record'
    Press-Element '^原文$'
    Press-Element '^键名$'
    $field=$window.FindFirst('Descendants',$editFilter)
    $field.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('中文')
    Press-Key 0x72
    $null=Find-Visible '第 1 处匹配'
    $null=Find-Visible 'field\s+中文'
    Press-Element '^取消搜索$'
    $null=Find-Visible '搜索已取消'
    Write-Output 'PASS escaped value search, jump and cancellation'
    Open-Sample $bad
    $null=Find-Visible ([regex]::Escape($bad))
    Press-Element '▸\s+\$\s+\{'
    $null=Find-Visible 'ok\s+true'
    $null=Find-Visible '字节 27'
    Open-Sample $utf8
    $null=Find-Visible '无效的 UTF-8'
    Write-Output 'PASS truncated JSON and invalid UTF-8'
    if($Stress){
        $output=[IO.File]::OpenWrite($wide)
        try{
            $output.Write([Text.Encoding]::UTF8.GetBytes('{"header":"ready","items":['))
            $chunk=[Text.Encoding]::ASCII.GetBytes('0,'*32768)
            foreach($i in 1..2048){$output.Write($chunk)}
            $output.Write([Text.Encoding]::ASCII.GetBytes('0]}'))
        }finally{$output.Dispose()}
        Open-Sample $wide
        $null=Find-Visible ([regex]::Escape($wide))
        Press-Element '^原文$'
        Start-Term 'absent-before-query-change'
        $null=Find-Visible '正在搜索'
        Click-Row '▸\s+\$\s+\{'
        Click-Row '▸\s+items\s+\['
        $row=(Find-Visible '\[12\]\s+0').Current.BoundingRectangle
        $point=([int]($row.Top+$row.Height/2) -shl 16) -bor ([int]($row.Left+70) -band 65535)
        foreach($i in 1..18){$null=[PreviewSmoke]::PostMessage($hwnd,0x20a,[IntPtr](-120 -shl 16),$point);Start-Sleep -Milliseconds 8}
        $null=Find-Visible '\[(5[0-9]|[6-9][0-9]|1[0-9][0-9])\]\s+0'
        $null=Find-Visible '正在搜索'
        Write-Output 'PASS expanding a subtree and scrolling beyond item 50 while search is active'
        Start-Term 'header'
        $null=Find-Visible 'header\s+ready'
        $null=Find-Visible '第 1 处匹配'
        Start-Term 'absent-before-file-change'
        $null=Find-Visible '正在搜索'
        Open-Sample $log
        $null=Find-Visible ([regex]::Escape($log))
        Start-Sleep -Milliseconds 400
        $null=Find-Visible '^就绪$'
        if($window.FindAll('Descendants',$any)|Where-Object {$_.Current.Name -match '扫描中\.json|absent-before|未找到匹配'}){
            throw 'A result from a cancelled document or query resurfaced'
        }
        Write-Output 'PASS replacing a pending query and file during a 128 MiB scan'
    }
    if($GiB){
        $output=[IO.File]::OpenWrite($huge)
        try{
            $output.Write([Text.Encoding]::UTF8.GetBytes('{"items":['))
            $chunk=[Text.Encoding]::ASCII.GetBytes((('"'+('x'*1021)+'",')*1024))
            foreach($i in 1..1024){$output.Write($chunk)}
            $output.Write([Text.Encoding]::ASCII.GetBytes('null],"end":"unique-gib-tail"}'))
        }finally{$output.Dispose()}
        Open-Sample $huge
        $null=Find-Visible ([regex]::Escape($huge))
        Press-Element '▸\s+\$\s+\{'
        $null=Find-Visible '▸\s+items\s+\['
        Press-Element '▸\s+items\s+\['
        $null=Find-Visible '\[0\]\s+xxx'
        Start-Term 'unique-gib-tail'
        $null=Find-Visible '第 1 处匹配'
        $null=Find-Visible 'end\s+unique-gib-tail'
        Write-Output 'PASS 1 GiB GUI open, first page of a million-item array, end-of-file hit and jump'
    }
    # Use the command-line entry for the locked file: the shell picker itself can reject a locked path.
    $start.ArgumentList.Clear();$start.ArgumentList.Add($locked)
    $errorProcess=[Diagnostics.Process]::Start($start)
    $null=$errorProcess.WaitForInputIdle(5000)
    $limit.Restart()
    do {$errorProcess.Refresh();if($errorProcess.MainWindowHandle){break};Start-Sleep -Milliseconds 10}while($limit.Elapsed.TotalSeconds -lt 5)
    $script:window=[System.Windows.Automation.AutomationElement]::FromHandle($errorProcess.MainWindowHandle)
    $null=Find-Visible 'os error 32|另一.*进程|另一个程序|another process'
    Write-Output 'PASS unreadable file feedback'
    $null=$errorProcess.CloseMainWindow()
    if(!$errorProcess.WaitForExit(2500)){$errorProcess.Kill();$errorProcess.WaitForExit()}
    $errorProcess.Dispose();$errorProcess=$null
    [IO.File]::WriteAllText($denied,'null')
    $originalAcl=Get-Acl -LiteralPath $denied
    $restricted=Get-Acl -LiteralPath $denied
    $sid=[Security.Principal.WindowsIdentity]::GetCurrent().User
    $restricted.AddAccessRule([Security.AccessControl.FileSystemAccessRule]::new($sid,'ReadData','Deny'))
    Set-Acl -LiteralPath $denied -AclObject $restricted
    $start.ArgumentList.Clear();$start.ArgumentList.Add($denied)
    $errorProcess=[Diagnostics.Process]::Start($start)
    $null=$errorProcess.WaitForInputIdle(5000)
    $limit.Restart()
    do{$errorProcess.Refresh();if($errorProcess.MainWindowHandle -ne [IntPtr]::Zero){break};Start-Sleep -Milliseconds 10}while($limit.Elapsed.TotalSeconds -lt 5)
    $script:window=[System.Windows.Automation.AutomationElement]::FromHandle($errorProcess.MainWindowHandle)
    $null=Find-Visible 'os error 5|拒绝访问|Access is denied'
    Write-Output 'PASS read permission denied with a fixture-only ACL'
} finally {
    if($originalAcl){Set-Acl -LiteralPath $denied -AclObject $originalAcl}
    if ($hold){$hold.Dispose()}
    foreach ($p in @($process,$errorProcess)) {
        if ($p){
            if(!$p.HasExited){$null=$p.CloseMainWindow();if(!$p.WaitForExit(2500)){$p.Kill();$p.WaitForExit()}}
            $p.Dispose()
        }
    }
    foreach ($file in @($log,$bad,$utf8,$locked,$denied,$wide,$huge)) {if([IO.File]::Exists($file)){[IO.File]::Delete($file)}}
    [IO.Directory]::Delete($temp)
}

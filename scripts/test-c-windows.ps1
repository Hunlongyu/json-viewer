#Requires -Version 7.0
param([string]$Viewer='target/c-win64/MinSizeRel/json-viewer.exe',[ValidateRange(1,20)][int]$Runs=1,[switch]$GiB,[switch]$Cold,[switch]$CheckForUpdate,
    [string[]]$Samples=@('(empty)','23MB.json','30MB.json','sdk-events_s.jsonl','sdk-events_x.jsonl','escaped.jsonl'))
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$exe=if([IO.Path]::IsPathRooted($Viewer)){(Get-Item -LiteralPath $Viewer).FullName}else{(Get-Item -LiteralPath (Join-Path $root $Viewer)).FullName}
Add-Type -Path (Join-Path $PSScriptRoot 'BenchmarkDesktop.cs')
if($Cold){Add-Type -Path (Join-Path $PSScriptRoot 'ColdSample.cs')}
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class PreviewControls {
    [ComImport,Guid("618736e0-3c3d-11cf-810c-00aa00389b71"),InterfaceType(ComInterfaceType.InterfaceIsIDispatch)]
    public interface IAcc { [DispId(-5004)] string this[object child] { [return:MarshalAs(UnmanagedType.BStr)] get; } }
    [DllImport("oleacc")] static extern int AccessibleObjectFromWindow(IntPtr hwnd,uint obj,ref Guid iid,[MarshalAs(UnmanagedType.Interface)] out IAcc acc);
    [DllImport("user32")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
    [DllImport("user32")] public static extern IntPtr GetMenu(IntPtr h);
    [DllImport("user32")] public static extern IntPtr GetSubMenu(IntPtr h,int index);
    [DllImport("user32")] public static extern int GetMenuItemID(IntPtr h,int index);
    [DllImport("user32")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32")] static extern bool GetClientRect(IntPtr h,out Rect r);
    [DllImport("user32")] static extern uint GetDpiForWindow(IntPtr h);
    [StructLayout(LayoutKind.Sequential)] struct Rect {public int left,top,right,bottom;}
    [DllImport("user32",EntryPoint="PostMessageW")] public static extern bool Post(IntPtr h,uint msg,IntPtr w,IntPtr l);
    public static string Detail(IntPtr hwnd) {
        var id=typeof(IAcc).GUID;IAcc acc;
        if(AccessibleObjectFromWindow(hwnd,0xfffffffc,ref id,out acc)<0 || acc==null)return "";
        try{return acc[0]??"";}catch(COMException){return "";}finally{Marshal.ReleaseComObject(acc);}
    }
    public static void Click(IntPtr hwnd,int x,int y) {
        Rect r;GetClientRect(hwnd,out r);double scale=GetDpiForWindow(hwnd)/96.0;
        int px=x<0?r.right+(int)(x*scale):(int)(x*scale);
        var point=(IntPtr)(((int)(y*scale)<<16)|px);
        Post(hwnd,512,IntPtr.Zero,point);System.Threading.Thread.Sleep(22);
        Post(hwnd,513,(IntPtr)1,point);System.Threading.Thread.Sleep(22);
        Post(hwnd,514,IntPtr.Zero,point);System.Threading.Thread.Sleep(28);
    }
    public static void Type(IntPtr hwnd,string s) {
        foreach(char c in s)Post(hwnd,258,(IntPtr)c,IntPtr.Zero);
    }
    public static void Key(IntPtr hwnd,int vk) {
        Post(hwnd,256,(IntPtr)vk,IntPtr.Zero);Post(hwnd,257,(IntPtr)vk,IntPtr.Zero);
    }
}
'@
function Wait-Text($hw,[string]$pattern){
    $wait=[Diagnostics.Stopwatch]::StartNew()
    do{
        $s=[PreviewControls]::Detail($hw)
        if($s -match $pattern -and [PreviewControls]::IsWindowVisible($hw)){return $s}
        Start-Sleep -Milliseconds 3
    }while($wait.Elapsed.TotalSeconds -lt 20)
    throw "Timed out waiting for '$pattern'. Visible selection: $s"
}
function Search-Text($hw,[string]$query){
    if($script:hasQuery){
        $cleared=$false
        for($attempt=0;$attempt -lt 3;$attempt++){
            [PreviewControls]::Click($hw,344,15)
            if([PreviewControls]::Detail($hw) -notmatch 'Find text \['){$cleared=$true;break}
        }
        if(!$cleared){throw "CUI search clear button did not clear: $([PreviewControls]::Detail($hw))"}
    }
    [PreviewControls]::Click($hw,150,15)
    [PreviewControls]::Type($hw,$query)
    $null=Wait-Text $hw ('Find text \['+[regex]::Escape($query)+'\]')
    $script:submitClock=[Diagnostics.Stopwatch]::StartNew()
    [PreviewControls]::Key($hw,13)
    $script:hasQuery=$true
}
function Down-To($hw,[string]$pattern){
    $watch=[Diagnostics.Stopwatch]::StartNew()
    do{
        $before=[PreviewControls]::Detail($hw)
        if($before -match $pattern){return}
        [PreviewControls]::Key($hw,40)
        $step=[Diagnostics.Stopwatch]::StartNew()
        $label=(($before -split '\r?\n')[-1] -split '\s+(?:byte|\d+ records)')[0]
        do{
            Start-Sleep -Milliseconds 3;$s=[PreviewControls]::Detail($hw)
            if($s -match $pattern){return}
            $current=(($s -split '\r?\n')[-1] -split '\s+(?:byte|\d+ records)')[0]
        }while($current -eq $label -and $step.ElapsedMilliseconds -lt 450)
        if($s -match $pattern){return}
    }while($watch.Elapsed.TotalSeconds -lt 5)
    throw "Could not select '$pattern': $s"
}
$temp=[IO.Directory]::CreateDirectory((Join-Path $root ('target/c-ui-'+[guid]::NewGuid().ToString('N'))))
try{
    $fixture=Join-Path $temp.FullName 'escaped.jsonl'
    [IO.File]::WriteAllText($fixture,([string][char]0xfeff)+"`r`n{broken}`r`n`r`n"+'{"field":"\u4e2d\u6587","field":"last","case_probe":"AbC73"}',[Text.UTF8Encoding]::new($false))
    if($GiB){
        $big=Join-Path $temp.FullName 'wide.json'
        $out=[IO.File]::Create($big)
        try{
            $head=[Text.Encoding]::UTF8.GetBytes('{"header":"ready","items":[');$out.Write($head)
            $row='"'+('x'*1021)+'",';$block=[Text.Encoding]::ASCII.GetBytes($row*1024)
            for($i=0;$i -lt 1024;$i++){$out.Write($block)}
            $out.Write([Text.Encoding]::ASCII.GetBytes('null],"end":"unique-gib-tail"}'))
        }finally{$out.Dispose()}
        $samples+='wide.json'
    }
    foreach($sample in $Samples){
        $source=if($sample -eq '(empty)'){$null}elseif($sample -in @('wide.json','escaped.jsonl')){Join-Path $temp.FullName $sample}else{Join-Path $root "bin/$sample"}
        $trials=if($sample -in @('wide.json','escaped.jsonl')){1}else{$Runs}
        for($run=1;$run -le $trials;$run++){
            $file=$source;$image=$exe
            if($Cold){
                if($source){$file=Join-Path $temp.FullName ("input-$run-"+[IO.Path]::GetFileName($source));[ColdSample]::Copy($source,$file)}
                $image=Join-Path $temp.FullName ("app-$run.exe")
                [ColdSample]::Copy($exe,$image)
            }
            $d=[BenchmarkDesktop]::new($image,$file)
            try{
                $hw=$d.WaitWindow('JSON Viewer')
                $script:hasQuery=$false
                foreach($id in @(101,102)){if([PreviewControls]::GetDlgItem($hw,$id) -ne [IntPtr]::Zero){throw 'Unexpected native text control'}}
                if(!$source){
                    $null=Wait-Text $hw 'Open or drop'
                    if ($CheckForUpdate) {
                        $help=[PreviewControls]::GetSubMenu([PreviewControls]::GetMenu($hw),3)
                        $command=[PreviewControls]::GetMenuItemID($help,0)
                        if ($command -lt 1){throw 'Missing Help > Check For Update'}
                        $null=[PreviewControls]::Post($hw,273,[IntPtr]$command,[IntPtr]::Zero)
                        $detail=Wait-Text $hw 'Check For Update\s+(No stable release|You are using the latest|An update is available|The new release has no matching Windows executable)'
                        Write-Host ($detail.Substring($detail.LastIndexOf('Check For Update')))
                    }
                    [pscustomobject]@{sample=$sample;run=$run;cold=[bool]$Cold;root_ms=[math]::Round($d.Clock.Elapsed.TotalMilliseconds,1)} | ConvertTo-Json -Compress
                    continue
                }
                $null=Wait-Text $hw 'Ready';$ready=$d.Clock.Elapsed.TotalMilliseconds
                if($sample -like '*.jsonl'){[PreviewControls]::Key($hw,39)}
                $load=$null;$first=$null;$last=$null;$search=$null;$tailSearch=$null
                if($sample -in @('23MB.json','30MB.json')){
                    $null=Wait-Text $hw '\bId\s+byte';$first=$d.Clock.Elapsed.TotalMilliseconds
                    $detail=Wait-Text $hw 'Load ([0-9.]+) ms'
                    $load=[double]([regex]::Match($detail,'Load ([0-9.]+) ms').Groups[1].Value)
                    if($sample -eq '30MB.json' -and $run -eq 1){
                        $null=Wait-Text $hw 'Count: 1,516,399'
                        [PreviewControls]::Click($hw,110,81);[PreviewControls]::Click($hw,110,81)
                        $null=Wait-Text $hw 'Runtime\s+byte[^\r\n]+· [1-9]\d* items'
                        [PreviewControls]::Click($hw,110,81);[PreviewControls]::Click($hw,110,81)
                        $null=Wait-Text $hw 'Runtime\s+byte[^\r\n]+\}\s+Count'
                        [PreviewControls]::Click($hw,6,81);[PreviewControls]::Click($hw,6,81)
                        $null=Wait-Text $hw 'Runtime\s+byte[^\r\n]+· [1-9]\d* items'
                        [PreviewControls]::Click($hw,6,81)
                        $null=Wait-Text $hw 'Runtime\s+byte[^\r\n]+\}\s+Count'
                    }
                    Down-To $hw 'Runtime\s+byte';[PreviewControls]::Key($hw,39);$null=Wait-Text $hw 'Runtime\s+byte[^\r\n]+· [1-9]\d* items';Down-To $hw 'Types\s+byte'
                    Search-Text $hw 'PlatformID';$null=Wait-Text $hw 'Match 1[^\r\n]*PlatformID';$search=$script:submitClock.Elapsed.TotalMilliseconds
                    [PreviewControls]::Key($hw,0x72);$null=Wait-Text $hw 'Match 2'
                    [PreviewControls]::Click($hw,370,15);$null=Wait-Text $hw 'Match 1'
                    [PreviewControls]::Click($hw,395,15);$null=Wait-Text $hw 'Match 2'
                    Search-Text $hw 'WSManStackVersion';$null=Wait-Text $hw 'Match 1[^\r\n]*WSManStackVersion';$tailSearch=$script:submitClock.Elapsed.TotalMilliseconds
                    if($sample -eq '30MB.json' -and $run -eq 1){
                        Search-Text $hw 'pwsh'
                        foreach($at in 1..4){
                            if($at -gt 1){[PreviewControls]::Key($hw,13)}
                            $offset=@(1505410,1505441,30772326,30772400)[$at-1]
                            $detail=Wait-Text $hw "(?s)Match $at · byte $offset .*Find: [0-9]+\.[0-9]{2} ms .*Find text \[pwsh\]\s+$at/4"
                            $elapsed=[regex]::Match($detail,'Find: [0-9.]+ ms').Value
                            if($at -eq 1){$findTime=$elapsed}
                            elseif($elapsed -ne $findTime){throw "Find time changed during navigation: $findTime -> $elapsed"}
                        }
                        $null=Wait-Text $hw 'Path\s+byte [0-9]+\s+C:'
                        [PreviewControls]::Key($hw,13);$null=Wait-Text $hw '(?s)Match 1 · byte 1505410 .*1/4'
                        [PreviewControls]::Key($hw,13);[PreviewControls]::Key($hw,13)
                        $null=Wait-Text $hw '(?s)Match 3 · byte 30772326 .*3/4'
                        [PreviewControls]::Click($hw,370,15);$null=Wait-Text $hw '(?s)Match 2 .*2/4'
                    }
                    Search-Text $hw 'absent-9fed2f88';$null=Wait-Text $hw '(?s)No matches.*Find: [0-9]+\.[0-9]{2} ms.*0/0'
                    [PreviewControls]::Click($hw,344,15);$null=Wait-Text $hw 'Load [0-9.]+ ms'
                }elseif($sample -like 'sdk*'){
                    $null=Wait-Text $hw 'JSONL\s+[1-9]\d* records.*expanded';[PreviewControls]::Key($hw,40);$null=Wait-Text $hw '#1 · line 1\s+byte';$first=$d.Clock.Elapsed.TotalMilliseconds
                    $count=if($sample -like '*_s*'){24325}else{50232}
                    [PreviewControls]::Key($hw,36);$null=Wait-Text $hw "$count records"
                    [PreviewControls]::Key($hw,35);$null=Wait-Text $hw "#$count · line";$last=$d.Clock.Elapsed.TotalMilliseconds
                    [PreviewControls]::Key($hw,39);$null=Wait-Text $hw '· [1-9]\d* items'
                    Search-Text $hw 'local_index';$null=Wait-Text $hw 'Match 1[^\r\n]*local_index';$search=$script:submitClock.Elapsed.TotalMilliseconds
                    $tailQuery=if($count -eq 24325){'1793953507517'}else{'1793958291119'}
                    Search-Text $hw $tailQuery;$null=Wait-Text $hw "Match 1[^\r\n]*$tailQuery";$tailSearch=$script:submitClock.Elapsed.TotalMilliseconds
                    if($count -eq 50232 -and $run -eq 1){$null=Wait-Text $hw 'Count: 602,784'}
                }elseif($sample -eq 'wide.json'){
                    $null=Wait-Text $hw 'header\s+byte';Down-To $hw 'items\s+byte'
                    [PreviewControls]::Key($hw,39);$null=Wait-Text $hw 'items\s+byte[^\r\n]+· [1-9]\d* items';Down-To $hw '\[0\]\s+byte'
                    Search-Text $hw 'unique-gib-tail';$null=Wait-Text $hw 'end\s+byte \d+\s+unique-gib-tail'
                }else{
                    Down-To $hw '#1 · line 2';[PreviewControls]::Key($hw,39);$null=Wait-Text $hw 'Line 2 · byte'
                    [PreviewControls]::Key($hw,35);$null=Wait-Text $hw '#2 · line 4'
                    [PreviewControls]::Key($hw,39);$null=Wait-Text $hw '· [1-9]\d* items';Down-To $hw 'field\s+byte \d+\s+中文'
                    [PreviewControls]::Key($hw,0x73);$null=Wait-Text $hw 'Search scope · Keys'
                    [PreviewControls]::Key($hw,0x73);$null=Wait-Text $hw 'Search scope · Values'
                    Search-Text $hw '中文';$null=Wait-Text $hw 'Match 1[^\r\n]*中文'
                    Search-Text $hw 'abc73';$null=Wait-Text $hw 'Match 1[^\r\n]*abc73'
                    [PreviewControls]::Click($hw,24,15);$null=Wait-Text $hw 'No matches'
                    [PreviewControls]::Click($hw,57,15)
                    Search-Text $hw 'AbC[0-9]{2}';$null=Wait-Text $hw 'Match 1[^\r\n]*AbC'
                    Search-Text $hw '[';$null=Wait-Text $hw '(?s)Find: —.*Find error'
                    [PreviewControls]::Key($hw,27);$null=Wait-Text $hw 'Search cancelled'
                }
                [pscustomobject]@{sample=$sample;run=$run;cold=[bool]$Cold;root_ms=[math]::Round($ready,1);first_ms=$first;load_ms=$load;last_ms=$last;first_search_ms=$search;tail_search_ms=$tailSearch} | ConvertTo-Json -Compress
            }finally{$d.Dispose();if($Cold){if($file){[IO.File]::Delete($file)};[IO.File]::Delete($image)}}
        }
    }
}finally{
    foreach($name in @('escaped.jsonl','wide.json')){[IO.File]::Delete((Join-Path $temp.FullName $name))}
    if(![IO.Directory]::EnumerateFileSystemEntries($temp.FullName).GetEnumerator().MoveNext()){[IO.Directory]::Delete($temp.FullName)}
}

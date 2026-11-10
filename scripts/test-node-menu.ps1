#Requires -Version 7.0
# Inspect actual Windows popup menus without changing the interactive clipboard.
param([string]$Viewer='dist/windows-x64/json-viewer.exe')
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
Add-Type -Path (Join-Path $PSScriptRoot 'BenchmarkDesktop.cs')
Add-Type @'
using System;using System.Text;using System.Runtime.InteropServices;using System.Threading;
public static class MenuTest {
 [DllImport("user32",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder b,int n);
 [DllImport("user32")] public static extern IntPtr SendMessageW(IntPtr h,uint msg,IntPtr w,IntPtr l);
 [DllImport("user32")] public static extern bool PostMessageW(IntPtr h,uint msg,IntPtr w,IntPtr l);
 [DllImport("user32")] public static extern int GetMenuItemCount(IntPtr m);
 [DllImport("user32")] public static extern IntPtr GetMenu(IntPtr h);
 [DllImport("user32")] public static extern int GetMenuItemID(IntPtr m,int i);
 [DllImport("user32")] public static extern IntPtr GetSubMenu(IntPtr m,int i);
 [DllImport("user32",CharSet=CharSet.Unicode)] public static extern int GetMenuString(IntPtr m,int i,StringBuilder s,int n,uint flags);
 public static void Right(IntPtr h,int x,int y){
     var pt=(IntPtr)(y*65536+x);PostMessageW(h,512,IntPtr.Zero,pt);Thread.Sleep(70);
     PostMessageW(h,516,(IntPtr)2,pt);Thread.Sleep(70);PostMessageW(h,517,IntPtr.Zero,pt);
 }
}
'@
function Key($h,$vk){$null=[MenuTest]::PostMessageW($h,256,[IntPtr]$vk,[IntPtr]::Zero);$null=[MenuTest]::PostMessageW($h,257,[IntPtr]$vk,[IntPtr]::Zero)}
function MenuNames($menu){
    for($i=0;$i -lt [MenuTest]::GetMenuItemCount($menu);$i++){
        $s=[Text.StringBuilder]::new(120);$null=[MenuTest]::GetMenuString($menu,$i,$s,120,1024)
        if($s.Length){$s.ToString()}
    }
}
function Context($y){
    [MenuTest]::Right($hw,100,$y);$watch=[Diagnostics.Stopwatch]::StartNew()
    do{
        foreach($w in $desktop.VisibleWindows()){
            $cls=[Text.StringBuilder]::new(60);$null=[MenuTest]::GetClassName($w,$cls,60)
            if($cls.ToString() -eq '#32768'){return [MenuTest]::SendMessageW($w,0x01e1,[IntPtr]::Zero,[IntPtr]::Zero)}
        }
        Start-Sleep -Milliseconds 10
    }while($watch.ElapsedMilliseconds -lt 2500)
    throw "No context menu for row at $y"
}
$testFile=Join-Path $project ('target/menu-'+[guid]::NewGuid().ToString('N')+'.json')
try{
    [IO.File]::WriteAllText($testFile,'{"field":"long\u4e2d value","arr":["entry"],"obj":{"a":1}}')
    $desktop=[BenchmarkDesktop]::new((Resolve-Path $Viewer).Path,$testFile)
    $hw=$desktop.WaitWindow('JSON Viewer');Start-Sleep -Milliseconds 230
    $bar=[MenuTest]::GetMenu($hw)
    if(((MenuNames $bar) -replace '&','') -join ',' -ne 'File,Edit,View,Help'){throw 'Unexpected main menu'}
    $file=[MenuTest]::GetSubMenu($bar,0)
    if((MenuNames ([MenuTest]::GetSubMenu($file,3))) -notcontains $testFile){throw 'Missing current file in Open Recent'}
    if((MenuNames ([MenuTest]::GetSubMenu($file,5))) -join ',' -ne 'JSON,Minified JSON,Formatted JSON'){throw 'Missing document exports'}
    $view=[MenuTest]::GetSubMenu($bar,2)
    foreach($i in 0,1){
        $id=[MenuTest]::GetMenuItemID($view,$i)
        $null=[MenuTest]::SendMessageW($hw,273,[IntPtr]$id,[IntPtr]::Zero)
        $null=[MenuTest]::SendMessageW($hw,278,$bar,[IntPtr]::Zero)
        $expected=@('Show Status Bar','Hide Node Path')[$i]
        if((MenuNames $view) -notcontains $expected){throw "Missing toggle: $expected"}
        $null=[MenuTest]::SendMessageW($hw,273,[IntPtr]$id,[IntPtr]::Zero)
    }
    $menu=Context 40
    if((MenuNames $menu) -join ',' -ne 'Copy Name,Copy Value,Copy Path'){throw 'Incorrect named scalar menu'}
    Key $hw 27;Start-Sleep -Milliseconds 120
    $menu=Context 61
    if((MenuNames $menu) -join ',' -ne 'Copy Name,Copy Value,Copy Value As,Copy Path,Export Value As'){throw 'Incorrect container menu'}
    if((MenuNames ([MenuTest]::GetSubMenu($menu,2))) -join ',' -ne 'Minified Value,Formatted Value'){throw 'Incorrect format choices'}
    Key $hw 27;Start-Sleep -Milliseconds 100;Key $hw 39;Start-Sleep -Milliseconds 180
    $menu=Context 82
    if((MenuNames $menu) -join ',' -ne 'Copy Value,Copy Path'){throw 'Incorrect array element menu'}
    Key $hw 27
    'Passed main menus, recent file, export formats, view toggles and node context menus.'
}finally{
    if($desktop){$desktop.Dispose()}
    [IO.File]::Delete($testFile)
}

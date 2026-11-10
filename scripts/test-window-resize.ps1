#Requires -Version 7.0
param([string]$Viewer='dist/windows-x64/json-viewer.exe',[switch]$Observe,[string]$Output='target/resize-check')
$ErrorActionPreference='Stop'
$project=Split-Path $PSScriptRoot -Parent
$out=[IO.Directory]::CreateDirectory((Join-Path $project $Output))
Add-Type -Path (Join-Path $PSScriptRoot 'BenchmarkDesktop.cs')
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;using System.Runtime.InteropServices;using System.Drawing;
public static class ResizeProbe {
 [DllImport("user32")] public static extern bool GetClientRect(IntPtr h,out Rectangle r);
 [DllImport("user32")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint flags);
 [DllImport("user32")] public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32")] public static extern IntPtr SendMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32")] public static extern bool PostMessageW(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32")] public static extern bool ShowWindowAsync(IntPtr h,int cmd);
 [DllImport("user32")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,System.Text.StringBuilder name,int size);
 [DllImport("user32")] public static extern uint GetWindowThreadProcessId(IntPtr h,IntPtr pid);
 [StructLayout(LayoutKind.Sequential)] public struct Gui {public uint size,flags;public IntPtr active,focus,capture,menu,move,caret;public Rectangle rect;}
 [DllImport("user32")] public static extern bool GetGUIThreadInfo(uint thread,ref Gui g);
 public static bool Moving(IntPtr h){var g=new Gui{size=(uint)Marshal.SizeOf<Gui>()};return GetGUIThreadInfo(GetWindowThreadProcessId(h,IntPtr.Zero),ref g)&&(g.flags&2)!=0;}
 public static void Key(IntPtr h,int vk){PostMessageW(h,256,(IntPtr)vk,IntPtr.Zero);PostMessageW(h,257,(IntPtr)vk,IntPtr.Zero);}
 [DllImport("user32")] public static extern IntPtr GetMenu(IntPtr h);
 [DllImport("user32")] public static extern IntPtr GetSubMenu(IntPtr menu,int index);
 [DllImport("user32")] public static extern int GetMenuItemID(IntPtr menu,int index);
 public static void Click(IntPtr h,int x,int y){
    var s=GetDpiForWindow(h)/96.0;var point=(IntPtr)(((int)(y*s)<<16)|(int)(x*s));
    PostMessageW(h,512,IntPtr.Zero,point);System.Threading.Thread.Sleep(35);
    PostMessageW(h,513,(IntPtr)1,point);System.Threading.Thread.Sleep(35);
    PostMessageW(h,514,IntPtr.Zero,point);System.Threading.Thread.Sleep(80);
 }
}
'@ -ReferencedAssemblies @('System.Drawing.Primitives','System.Threading.Thread')
function Capture([string]$name){
    $r=[Drawing.Rectangle]::Empty;$null=[ResizeProbe]::GetClientRect($hw,[ref]$r)
    $bmp=[Drawing.Bitmap]::new($r.Width,$r.Height);$g=[Drawing.Graphics]::FromImage($bmp);$dc=$g.GetHdc()
    try{if(![ResizeProbe]::PrintWindow($hw,$dc,3)){throw 'Window capture failed'}}finally{$g.ReleaseHdc($dc);$g.Dispose()}
    try{
        $path=Join-Path $out "$name.png";$bmp.Save($path)
        $black=0;$total=0;$bottomInk=0
        for($y=1;$y -lt $r.Height;$y+=4){for($x=1;$x -lt $r.Width;$x+=4){
            $c=$bmp.GetPixel($x,$y);$total++
            if($c.R -lt 16 -and $c.G -lt 16 -and $c.B -lt 16){$black++}
            if($y -gt $r.Height-25 -and $c.R -lt 170 -and $c.G -lt 170 -and $c.B -lt 170){$bottomInk++}
        }}
        $percent=[math]::Round(100*$black/$total,2)
        [pscustomobject]@{step=$name;width=$r.Width;height=$r.Height;modal=[ResizeProbe]::Moving($hw);black_pct=$percent;status_pixels=$bottomInk}
        if(!$Observe -and ($percent -gt 4 -or $bottomInk -lt 5)){throw "Missing status bar or incomplete paint at $name"}
    }finally{$bmp.Dispose()}
}
$exe=(Resolve-Path $Viewer).Path;$d=[BenchmarkDesktop]::new($exe,(Join-Path $project 'bin/30MB.json'))
try{
    $hw=$d.WaitWindow('JSON Viewer');Start-Sleep -Milliseconds 250
    foreach($i in 1..2){[ResizeProbe]::Key($hw,40);Start-Sleep -Milliseconds 30}
    [ResizeProbe]::Key($hw,39);Start-Sleep -Milliseconds 180
    Capture 'startup'
    [ResizeProbe]::Key($hw,40);[ResizeProbe]::Key($hw,39);Start-Sleep -Milliseconds 180
    [ResizeProbe]::Key($hw,40);[ResizeProbe]::Key($hw,39);Start-Sleep -Milliseconds 180
    Capture 'expanded'
    $client=[Drawing.Rectangle]::Empty;$null=[ResizeProbe]::GetClientRect($hw,[ref]$client)
    $edge=[IntPtr]((150 -shl 16) -bor ($client.Width-5))
    $null=[ResizeProbe]::PostMessageW($hw,512,[IntPtr]::Zero,$edge)
    Start-Sleep -Milliseconds 200;Capture 'expanded-scroll'
    if(!$Observe){
        # Exercise the menu itself, not the duplicate toolbar shortcut.
        $fileMenu=[ResizeProbe]::GetSubMenu([ResizeProbe]::GetMenu($hw),0)
        $open=[ResizeProbe]::GetMenuItemID($fileMenu,1)
        if ($open -lt 1){throw 'File > Open is missing'}
        $null=[ResizeProbe]::PostMessageW($hw,273,[IntPtr]$open,[IntPtr]::Zero)
        $watch=[Diagnostics.Stopwatch]::StartNew();$picker=[IntPtr]::Zero
        do{
            foreach($w in $d.VisibleWindows()){
                $cls=[Text.StringBuilder]::new(80);$null=[ResizeProbe]::GetClassName($w,$cls,80)
                if($cls.ToString() -eq '#32770'){$picker=$w;break}
            }
            if(!$picker.ToInt64()){Start-Sleep -Milliseconds 25}
        }while(!$picker.ToInt64() -and $watch.ElapsedMilliseconds -lt 5000)
        if(!$picker.ToInt64()){throw 'File > Open did not show the file picker'}
        $null=[ResizeProbe]::PostMessageW($picker,16,[IntPtr]::Zero,[IntPtr]::Zero)
        Start-Sleep -Milliseconds 180
    }
    $null=[ResizeProbe]::PostMessageW($hw,274,[IntPtr]0xf008,[IntPtr]::Zero)
    $watch=[Diagnostics.Stopwatch]::StartNew()
    while(![ResizeProbe]::Moving($hw) -and $watch.ElapsedMilliseconds -lt 1000){Start-Sleep -Milliseconds 10}
    # Inactive desktops may not grant system mouse capture. Deliver the same
    # sizing notifications and change the real HWND dimensions in that case.
    if(![ResizeProbe]::Moving($hw)){$null=[ResizeProbe]::SendMessageW($hw,0x231,[IntPtr]::Zero,[IntPtr]::Zero)}
    foreach($size in @(@(360,440),@(1080,710),@(480,580),@(980,750))){
        $null=[ResizeProbe]::SetWindowPos($hw,[IntPtr]::Zero,0,0,$size[0],$size[1],0x16)
        Start-Sleep -Milliseconds 45
        Capture "drag-$($size[0])"
    }
    $null=[ResizeProbe]::SendMessageW($hw,0x232,[IntPtr]::Zero,[IntPtr]::Zero);[ResizeProbe]::Key($hw,13);Start-Sleep -Milliseconds 120
    $null=[ResizeProbe]::ShowWindowAsync($hw,3);Start-Sleep -Milliseconds 170;Capture 'maximized'
    $null=[ResizeProbe]::ShowWindowAsync($hw,6);Start-Sleep -Milliseconds 80
    $null=[ResizeProbe]::ShowWindowAsync($hw,9);Start-Sleep -Milliseconds 170;Capture 'restored'
    $null=[ResizeProbe]::ShowWindowAsync($hw,9);Start-Sleep -Milliseconds 170;Capture 'normal'
}finally{$d.Dispose()}

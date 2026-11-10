#Requires -Version 7.0
param([string]$Viewer='dist/windows-x64/json-viewer.exe')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
Add-Type -Path (Join-Path $PSScriptRoot 'BenchmarkDesktop.cs')
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Drawing;
using System.Text;
using System.Runtime.InteropServices;
public static class ScrollProbe {
    [DllImport("user32")] public static extern bool GetClientRect(IntPtr h, out Rectangle r);
    [DllImport("user32")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
    [DllImport("user32")] public static extern uint GetDpiForWindow(IntPtr h);
    [DllImport("user32")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32")] public static extern IntPtr GetMenu(IntPtr h);
    [DllImport("user32")] public static extern IntPtr GetSubMenu(IntPtr h, int index);
    [DllImport("user32")] public static extern int GetMenuItemCount(IntPtr h);
    [DllImport("user32")] public static extern int GetMenuItemID(IntPtr h, int index);
    [DllImport("user32", CharSet=CharSet.Unicode)] public static extern int GetMenuStringW(IntPtr h, int index, StringBuilder text, int count, uint flags);
    [ComImport, Guid("618736e0-3c3d-11cf-810c-00aa00389b71"), InterfaceType(ComInterfaceType.InterfaceIsIDispatch)]
    public interface IAcc { [DispId(-5004)] string this[object child] { [return:MarshalAs(UnmanagedType.BStr)] get; } }
    [DllImport("oleacc")] static extern int AccessibleObjectFromWindow(IntPtr h, uint id, ref Guid iid, [MarshalAs(UnmanagedType.Interface)] out IAcc acc);
    public static string Selection(IntPtr h) {
        Guid iid = typeof(IAcc).GUID;
        if (AccessibleObjectFromWindow(h, 0xfffffffc, ref iid, out IAcc acc) < 0 || acc == null) return "";
        try { return acc[0] ?? ""; } finally { Marshal.ReleaseComObject(acc); }
    }
}
"@ -ReferencedAssemblies 'System.Drawing.Primitives'

function Wait-Selection($hw,[string]$pattern) {
    $timer=[Diagnostics.Stopwatch]::StartNew()
    do {
        $detail=[ScrollProbe]::Selection($hw)
        if ($detail -match $pattern) {return}
        Start-Sleep -Milliseconds 20
    } while ($timer.Elapsed.TotalSeconds -lt 10)
    throw "Expected '$pattern', got '$detail'"
}

# Compare the actual tree pixels, including the scrollbar gutter. The title,
# toolbar and status contain focus state or file-specific timing information.
function Tree-Hash($hw) {
    $r=[Drawing.Rectangle]::Empty
    $null=[ScrollProbe]::GetClientRect($hw,[ref]$r)
    $bmp=[Drawing.Bitmap]::new($r.Width,$r.Height)
    try {
        $g=[Drawing.Graphics]::FromImage($bmp)
        $dc=$g.GetHdc()
        try {
            if (![ScrollProbe]::PrintWindow($hw,$dc,3)) {throw 'Could not capture window'}
        } finally {$g.ReleaseHdc($dc); $g.Dispose()}
        $margin=[int][Math]::Ceiling(32*[ScrollProbe]::GetDpiForWindow($hw)/96)
        $area=[Drawing.Rectangle]::new(0,$margin,$r.Width,$r.Height-2*$margin)
        $tree=$bmp.Clone($area,$bmp.PixelFormat)
        $stream=[IO.MemoryStream]::new()
        try {
            $tree.Save($stream,[Drawing.Imaging.ImageFormat]::Png)
            [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream.ToArray()))
        } finally {$stream.Dispose(); $tree.Dispose()}
    } finally {$bmp.Dispose()}
}

$exe=(Resolve-Path $Viewer).Path
$prefix=Join-Path $root ('target/switch-'+[guid]::NewGuid().ToString('N'))
$small=$prefix+'.json'
$large=$prefix+'.jsonl'
try {
    [IO.File]::WriteAllText($small,'{"firstVisible":"top row","secondVisible":"bottom row"}')
    [IO.File]::WriteAllText($large,("42`n"*100000))
    $d=[BenchmarkDesktop]::new($exe,$small)
    try {
        $hw=$d.WaitWindow('JSON Viewer')
        Wait-Selection $hw 'firstVisible\s+byte.*Count: 2'
        $expected=Tree-Hash $hw
    } finally {$d.Dispose()}

    $d=[BenchmarkDesktop]::new($exe,$large)
    try {
        $hw=$d.WaitWindow('JSON Viewer')
        Wait-Selection $hw '100000 records'
        $null=[ScrollProbe]::PostMessageW($hw,256,39,0)
        $null=[ScrollProbe]::PostMessageW($hw,257,39,0)
        Wait-Selection $hw 'expanded'
        $null=[ScrollProbe]::PostMessageW($hw,256,35,0)
        $null=[ScrollProbe]::PostMessageW($hw,257,35,0)
        Wait-Selection $hw '#100000 · line 100000'
        Start-Sleep -Milliseconds 600
        $file=[ScrollProbe]::GetSubMenu([ScrollProbe]::GetMenu($hw),0)
        $recent=[ScrollProbe]::GetSubMenu($file,3)
        $name=[Text.StringBuilder]::new(1024)
        $command=-1
        for ($i=0; $i -lt [ScrollProbe]::GetMenuItemCount($recent); $i++) {
            $null=[ScrollProbe]::GetMenuStringW($recent,$i,$name,$name.Capacity,1024)
            if ($name.ToString().Replace('&&','&') -eq $small) {
                $command=[ScrollProbe]::GetMenuItemID($recent,$i)
                break
            }
        }
        if ($command -lt 0) {throw "Missing recent file: $small"}
        $null=[ScrollProbe]::PostMessageW($hw,273,$command,0)
        Wait-Selection $hw 'firstVisible\s+byte.*Count: 2'
        $timer=[Diagnostics.Stopwatch]::StartNew()
        while ((Tree-Hash $hw) -ne $expected) {
            if ($timer.ElapsedMilliseconds -gt 500) {throw 'Small JSON loaded but its tree did not repaint at the top'}
            Start-Sleep -Milliseconds 15
        }
        'Passed document switch from last JSONL record to fully visible small JSON.'
    } finally {$d.Dispose()}
} finally {
    [IO.File]::Delete($small)
    [IO.File]::Delete($large)
}

using System;
using System.ComponentModel;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

// Dadroit forwards its command line to a window on the same desktop.
// A private, never-activated desktop keeps each trial in its own process.
public sealed class BenchmarkDesktop : IDisposable {
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct Startup {
        public int Size;
        public string Reserved, Desktop, Title;
        public int X,Y,Width,Height,XChars,YChars,Fill,Flags;
        public short Show,ReservedSize;
        public IntPtr ReservedData,Input,Output,Error;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct Created { public IntPtr Process,Thread; public uint ProcessId,ThreadId; }
    delegate bool Visit(IntPtr hwnd,IntPtr state);
    [DllImport("user32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern IntPtr CreateDesktop(string name,IntPtr device,IntPtr mode,int flags,uint access,IntPtr security);
    [DllImport("user32.dll")] static extern bool CloseDesktop(IntPtr desktop);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern bool CreateProcess(string app,StringBuilder args,IntPtr processSecurity,IntPtr threadSecurity,
        bool inherit,int flags,IntPtr environment,string cwd,ref Startup startup,out Created created);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("user32.dll")] static extern bool EnumDesktopWindows(IntPtr desktop,Visit visit,IntPtr state);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr hwnd);
    [DllImport("user32.dll",CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr hwnd,StringBuilder text,int size);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr hwnd,out uint id);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr hwnd,uint msg,IntPtr w,IntPtr l);
    public readonly Process Process;
    public readonly Stopwatch Clock;
    public IntPtr Window {get;private set;}
    readonly IntPtr desktop;
    public BenchmarkDesktop(string exe,string file) {
        string name="json_preview_"+Guid.NewGuid().ToString("N");
        desktop=CreateDesktop(name,IntPtr.Zero,IntPtr.Zero,0,0x1ff,IntPtr.Zero);
        if(desktop==IntPtr.Zero)throw new Win32Exception();
        var startup=new Startup{Size=Marshal.SizeOf<Startup>(),Desktop="winsta0\\"+name};
        var args=new StringBuilder("\""+exe+"\"");
        if(!String.IsNullOrEmpty(file)) args.Append(" \"").Append(file).Append('"');
        Clock=Stopwatch.StartNew();
        if(!CreateProcess(exe,args,IntPtr.Zero,IntPtr.Zero,false,0,IntPtr.Zero,
                          Path.GetDirectoryName(exe),ref startup,out Created created)){
            var e=new Win32Exception();CloseDesktop(desktop);throw e;
        }
        try {Process=Process.GetProcessById((int)created.ProcessId);}
        finally {CloseHandle(created.Thread);CloseHandle(created.Process);}
    }
    public IntPtr WaitWindow(string titleFragment) {
        var wait=Stopwatch.StartNew();
        while(wait.ElapsedMilliseconds<15000 && !Process.HasExited) {
            EnumDesktopWindows(desktop,(h,unused)=>{
                GetWindowThreadProcessId(h,out uint id);
                if(id!=Process.Id || !IsWindowVisible(h))return true;
                var text=new StringBuilder(512);
                GetWindowText(h,text,text.Capacity);
                if(!text.ToString().Contains(titleFragment))return true;
                Window=h;return false;
            },IntPtr.Zero);
            if(Window!=IntPtr.Zero)return Window;
            Thread.Sleep(3);
        }
        throw new InvalidOperationException("No owned window with title "+titleFragment);
    }
    public IntPtr[] VisibleWindows() {
        var result=new List<IntPtr>();
        EnumDesktopWindows(desktop,(h,unused)=>{
            GetWindowThreadProcessId(h,out uint id);
            if(id==Process.Id && IsWindowVisible(h))result.Add(h);
            return true;
        },IntPtr.Zero);
        return result.ToArray();
    }
    public void Dispose() {
        try {
            if(Window!=IntPtr.Zero)PostMessage(Window,0x10,IntPtr.Zero,IntPtr.Zero);
            if(!Process.HasExited && !Process.WaitForExit(1600)){Process.Kill();Process.WaitForExit();}
            Process.Dispose();
        }finally{CloseDesktop(desktop);}
    }
}

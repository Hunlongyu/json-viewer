using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

// Populate a new benchmark file without putting its contents into the Windows
// file cache. No system-wide cache purge or change to the source is required.
public static class ColdSample {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    private static extern SafeFileHandle CreateFileW(string path, uint access, uint share, IntPtr security,
        uint disposition, uint flags, IntPtr template);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool WriteFile(SafeFileHandle file, IntPtr data, int size, out int written, IntPtr overlapped);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern IntPtr VirtualAlloc(IntPtr address, UIntPtr size, uint allocationType, uint protect);
    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool VirtualFree(IntPtr data, UIntPtr size, uint freeType);

    public static void Copy(string source, string destination) {
        const int size = 1024 * 1024;
        const int sector = 4096;
        var block = new byte[size];
        long length;
        using (var input = File.OpenRead(source))
        using (var output = CreateFileW(destination, 0x40000000, 0, IntPtr.Zero, 1,
                   0x20000000 | 0x80000000, IntPtr.Zero)) { // CREATE_NEW, NO_BUFFERING | WRITE_THROUGH
            if (output.IsInvalid) throw new Win32Exception(Marshal.GetLastWin32Error());
            length = input.Length;
            var aligned = VirtualAlloc(IntPtr.Zero, (UIntPtr)size, 0x3000, 4);
            if (aligned == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error());
            try {
                for (long pos = 0; pos < length;) {
                    int n = (int)Math.Min(size, length - pos);
                    input.ReadExactly(block, 0, n);
                    int padded = (n + sector - 1) / sector * sector;
                    Array.Clear(block, n, padded - n);
                    Marshal.Copy(block, 0, aligned, padded);
                    if (!WriteFile(output, aligned, padded, out int written, IntPtr.Zero))
                        throw new Win32Exception(Marshal.GetLastWin32Error());
                    if (written != padded) throw new IOException("Incomplete unbuffered write");
                    pos += n;
                }
            } finally {
                VirtualFree(aligned, UIntPtr.Zero, 0x8000);
            }
        }
        // Truncate alignment padding without reading the destination's data.
        using (var file = File.OpenWrite(destination)) file.SetLength(length);
    }
}

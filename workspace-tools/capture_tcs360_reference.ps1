param(
    [Parameter(Mandatory=$true)][int]$ReferenceProcessId,
    [Parameter(Mandatory=$true)][string]$ExpectedExecutable,
    [Parameter(Mandatory=$true)][datetime]$ExpectedStartUtc,
    [Parameter(Mandatory=$true)][string]$OutputPath
)
# Read only the game image from the specifically launched Xenia reference.
# No process-name discovery, injection, suspension, or memory writes.
$ErrorActionPreference = 'Stop'
if (Test-Path -LiteralPath $OutputPath) { throw 'Refusing to overwrite output' }
$reference = Get-Process -Id $ReferenceProcessId
$expectedPath = (Resolve-Path -LiteralPath $ExpectedExecutable).Path
if ($reference.Path -ine $expectedPath -or
    [IO.Path]::GetFileName($expectedPath) -ine 'xenia_canary.exe' -or
    [Math]::Abs(($reference.StartTime.ToUniversalTime() - $ExpectedStartUtc.ToUniversalTime()).TotalMilliseconds) -gt 1) {
    throw 'Reference process identity/start time does not match'
}
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class TcsReferenceReader {
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern IntPtr OpenProcess(uint access, bool inherit, uint processId);
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern bool ReadProcessMemory(IntPtr process, IntPtr address,
        [Out] byte[] buffer, UIntPtr size, out UIntPtr read);
    [DllImport("kernel32.dll")]
    public static extern bool CloseHandle(IntPtr process);
    [StructLayout(LayoutKind.Sequential)]
    public struct Region {
        public ulong Base, AllocationBase;
        public uint AllocationProtect, Align1;
        public ulong Size;
        public uint State, Protect, Type, Align2;
    }
    [DllImport("kernel32.dll", SetLastError=true)]
    public static extern UIntPtr VirtualQueryEx(IntPtr process, IntPtr address,
        out Region region, UIntPtr length);
    // First 64 KiB-aligned committed address holding a PE header, at or after start.
    public static long FindImage(IntPtr process, long start, long end) {
        byte[] head = new byte[2];
        long at = start;
        while (at < end) {
            Region r;
            if (VirtualQueryEx(process, new IntPtr(at), out r,
                    new UIntPtr((uint)Marshal.SizeOf(typeof(Region)))) == UIntPtr.Zero) return -1;
            long next = (long)r.Base + (long)r.Size;
            const uint Commit = 0x1000, NoAccess = 0x01, Guard = 0x100;
            if (r.State == Commit && (r.Protect & (NoAccess | Guard)) == 0) {
                for (long p = (at + 0xFFFF) & ~0xFFFFL; p + 2 <= next; p += 0x10000) {
                    UIntPtr got;
                    if (ReadProcessMemory(process, new IntPtr(p), head, new UIntPtr(2u), out got) &&
                        got.ToUInt64() == 2 && head[0] == 0x4D && head[1] == 0x5A) return p;
                }
            }
            at = next;
        }
        return -1;
    }
}
'@
$handle = [TcsReferenceReader]::OpenProcess(0x410, $false, $ReferenceProcessId)
if ($handle -eq [IntPtr]::Zero) { throw 'Cannot open owned reference for reading' }
$imageSize = 0x1460000
$guestBase = 0x82000000L
# Xenia builds place guest memory differently, so search for the PE header and let the
# code hash below decide. Only a candidate whose whole image reads and matches is saved.
$image = New-Object byte[] $imageSize
$imageAddress = 0L
$expectedText = 'c783e25afc226be7a63de0ce615b74435ce759693ee68e533b0018fc53ff2079'
$textHash = $null
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $search = 0x10000L
    while (-not $imageAddress) {
        $candidate = [TcsReferenceReader]::FindImage($handle, $search, 0x800000000L)
        if ($candidate -lt 0) { throw 'No guest image with the expected code hash; no output saved' }
        $search = $candidate + 0x10000L
        $complete = $true
        for ($offset = 0; $offset -lt $imageSize; $offset += 0x10000) {
            $size = [Math]::Min(0x10000, $imageSize - $offset)
            $chunk = New-Object byte[] $size
            $read = [UIntPtr]::Zero
            $ok = [TcsReferenceReader]::ReadProcessMemory($handle,
                [IntPtr]($candidate + $offset), $chunk, [UIntPtr][uint64]$size, [ref]$read)
            if (-not $ok -or $read.ToUInt64() -ne $size) { $complete = $false; break }
            [Buffer]::BlockCopy($chunk, 0, $image, $offset, $size)
        }
        if (-not $complete) { continue }
        $textHash = [BitConverter]::ToString($sha.ComputeHash($image, 0x13E600, 0xC1302C)).Replace('-', '').ToLowerInvariant()
        if ($textHash -eq $expectedText) { $imageAddress = $candidate }
    }
    $imageHash = [BitConverter]::ToString($sha.ComputeHash($image)).Replace('-', '').ToLowerInvariant()
} finally {
    $sha.Dispose()
    [void][TcsReferenceReader]::CloseHandle($handle)
}
$dest = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
try { $dest.Write($image, 0, $image.Length) } finally { $dest.Dispose() }
[pscustomobject]@{file=$OutputPath; bytes=$image.Length; sha256=$imageHash;
    text_sha256=$textHash; reference_pid=$ReferenceProcessId;
    host_address=('0x{0:X}' -f $imageAddress);
    note='Reference snapshot; validate import fixups and section differences before runtime use'} | ConvertTo-Json

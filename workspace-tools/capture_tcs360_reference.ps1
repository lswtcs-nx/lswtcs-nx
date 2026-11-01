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
}
'@
$handle = [TcsReferenceReader]::OpenProcess(0x410, $false, $ReferenceProcessId)
if ($handle -eq [IntPtr]::Zero) { throw 'Cannot open owned reference for reading' }
$imageSize = 0x1460000
$guestBase = 0x82000000L
$hostOffset = 0x110000000L
$image = New-Object byte[] $imageSize
try {
    for ($offset = 0; $offset -lt $imageSize; $offset += 0x10000) {
        $size = [Math]::Min(0x10000, $imageSize - $offset)
        $chunk = New-Object byte[] $size
        $read = [UIntPtr]::Zero
        $ok = [TcsReferenceReader]::ReadProcessMemory($handle,
            [IntPtr]($hostOffset + $guestBase + $offset), $chunk, [UIntPtr][uint64]$size, [ref]$read)
        if (-not $ok -or $read.ToUInt64() -ne $size) {
            throw ('Incomplete reference read at image offset 0x{0:X}; no output saved' -f $offset)
        }
        [Buffer]::BlockCopy($chunk, 0, $image, $offset, $size)
    }
} finally {
    [void][TcsReferenceReader]::CloseHandle($handle)
}
if ($image[0] -ne 0x4D -or $image[1] -ne 0x5A) { throw 'Guest image is not PE/MZ' }
$sha = [Security.Cryptography.SHA256]::Create()
try {
    $textHash = [BitConverter]::ToString($sha.ComputeHash($image, 0x13E600, 0xC1302C)).Replace('-', '').ToLowerInvariant()
    if ($textHash -ne 'c783e25afc226be7a63de0ce615b74435ce759693ee68e533b0018fc53ff2079') {
        throw 'Guest code hash differs from the supplied Rev 1 executable; no output saved'
    }
    $imageHash = [BitConverter]::ToString($sha.ComputeHash($image)).Replace('-', '').ToLowerInvariant()
} finally { $sha.Dispose() }
$dest = [IO.File]::Open($OutputPath, [IO.FileMode]::CreateNew, [IO.FileAccess]::Write)
try { $dest.Write($image, 0, $image.Length) } finally { $dest.Dispose() }
[pscustomobject]@{file=$OutputPath; bytes=$image.Length; sha256=$imageHash;
    text_sha256=$textHash; reference_pid=$ReferenceProcessId;
    note='Reference snapshot; validate import fixups and section differences before runtime use'} | ConvertTo-Json

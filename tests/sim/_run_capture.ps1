# ===================================================================
#  Run tests\sim\sim.exe and capture stdout / stderr into UTF-8 files.
#
#  Why this helper exists:
#    * sim.exe uses the GUI subsystem (-Wl,--subsystem,windows), so a plain
#      PowerShell "& $exe" does not wait for it and collects no output;
#    * cmd /c rewrites the argument tail when the command line contains both
#      a redirection and an '=', so it cannot be used for verification either
#      (--api=dx11 arrives as "--api dx11" -- measured, not guessed);
#    * this script uses .NET Process with hand-rolled quoting, so every
#      argument arrives byte-for-byte and output is decoded as UTF-8.
#
#  usage:
#    powershell -File tests\sim\_run_capture.ps1 <msTimeout> <outFile> <errFile> <exe> [args...]
# ===================================================================
param(
    [Parameter(Mandatory = $true)][int]$TimeoutMs,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [Parameter(Mandatory = $true)][string]$ErrFile,
    [Parameter(Mandatory = $true)][string]$Exe,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$SimArgs
)

# Windows command line quoting rules (inverse of CommandLineToArgvW):
# backslashes only need doubling when they immediately precede a quote.
function Quote-Arg([string]$a) {
    if ($a -eq '') { return '""' }
    if ($a -notmatch '[\s"]') { return $a }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.Append('"')
    $bs = 0
    foreach ($ch in $a.ToCharArray()) {
        if ($ch -eq '\') { $bs++; continue }
        if ($ch -eq '"') {
            [void]$sb.Append('\' * (2 * $bs + 1))
            [void]$sb.Append('"')
            $bs = 0
            continue
        }
        if ($bs -gt 0) { [void]$sb.Append('\' * $bs); $bs = 0 }
        [void]$sb.Append($ch)
    }
    if ($bs -gt 0) { [void]$sb.Append('\' * (2 * $bs)) }
    [void]$sb.Append('"')
    return $sb.ToString()
}

$psi = New-Object System.Diagnostics.ProcessStartInfo
$psi.FileName = $Exe
$psi.UseShellExecute = $false
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.StandardOutputEncoding = New-Object System.Text.UTF8Encoding($false)
$psi.StandardErrorEncoding = New-Object System.Text.UTF8Encoding($false)
$psi.CreateNoWindow = $true
$quoted = @()
foreach ($a in $SimArgs) { $quoted += (Quote-Arg $a) }
$psi.Arguments = ($quoted -join ' ')

$p = [System.Diagnostics.Process]::Start($psi)
# Read asynchronously: otherwise a full pipe buffer deadlocks the child
# (very likely with --json, which writes one line per frame).
$so = $p.StandardOutput.ReadToEndAsync()
$se = $p.StandardError.ReadToEndAsync()
if (-not $p.WaitForExit($TimeoutMs)) {
    Write-Host "TIMEOUT after $TimeoutMs ms -- killing pid $($p.Id)"
    try { $p.Kill() } catch { }
    [void]$p.WaitForExit(3000)
}
[System.IO.File]::WriteAllText($OutFile, $so.Result, (New-Object System.Text.UTF8Encoding($false)))
[System.IO.File]::WriteAllText($ErrFile, $se.Result, (New-Object System.Text.UTF8Encoding($false)))
Write-Host "exit=$($p.ExitCode)  stdoutChars=$($so.Result.Length)  stderrChars=$($se.Result.Length)"

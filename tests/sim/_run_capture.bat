@echo off
rem ===================================================================
rem  Run sim.exe and capture stdout / stderr into files.
rem
rem  Why this exists: cmd.exe rewrites the argument tail when a command
rem  line contains a redirection *and* an '=' sign (it drops the '='),
rem  and PowerShell's Start-Process has its own quoting quirks. This tiny
rem  wrapper uses .NET ProcessStartInfo with an explicit ArgumentList, so
rem  the arguments arrive byte-for-byte and the output is captured
rem  reliably -- including the GUI-subsystem stdout handoff.
rem
rem  usage: run_capture.bat <msec-timeout> <stdout-file> <stderr-file> <sim.exe> [args...]
rem ===================================================================
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='Stop';" ^
  "$ms=[int]$args[0]; $o=$args[1]; $e=$args[2]; $exe=$args[3];" ^
  "$rest=@(); for($i=4;$i -lt $args.Count;$i++){ $rest += $args[$i] }" ^
  "$psi=New-Object System.Diagnostics.ProcessStartInfo;" ^
  "$psi.FileName=$exe; $psi.UseShellExecute=$false;" ^
  "$psi.RedirectStandardOutput=$true; $psi.RedirectStandardError=$true;" ^
  "$psi.StandardOutputEncoding=[System.Text.Encoding]::UTF8;" ^
  "$psi.StandardErrorEncoding=[System.Text.Encoding]::UTF8;" ^
  "foreach($a in $rest){ [void]$psi.ArgumentList.Add($a) }" ^
  "$p=[System.Diagnostics.Process]::Start($psi);" ^
  "$so=$p.StandardOutput.ReadToEndAsync(); $se=$p.StandardError.ReadToEndAsync();" ^
  "if(-not $p.WaitForExit($ms)){ Write-Host ('TIMEOUT after '+$ms+' ms'); $p.Kill() }" ^
  "[System.IO.File]::WriteAllText($o,$so.Result,[System.Text.Encoding]::UTF8);" ^
  "[System.IO.File]::WriteAllText($e,$se.Result,[System.Text.Encoding]::UTF8);" ^
  "Write-Host ('exit=' + $p.ExitCode)" ^
  -- %1 %2 %3 %4 %5 %6 %7 %8 %9 %10 %11 %12 %13 %14 %15
endlocal

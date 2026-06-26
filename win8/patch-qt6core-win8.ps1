<#
  patch-qt6core-win8.ps1 - make a redistributed Qt6Core.dll loadable on Windows 8.0/8.1.

  WHY: deskflow-core.exe itself imports nothing that is missing on Windows 8 (verified by
  diffing its import table against the actual kernel32/kernelbase/... exports pulled off a
  6.2.9200 box). The SOLE blocker is its Qt dependency: Qt 6.5+ (we ship 6.8.1) statically
  imports kernel32!SetThreadDescription, which only exists on Windows 10 1607+. On Win8 the
  loader fails the process with STATUS_ENTRYPOINT_NOT_FOUND (0xC0000139) before main() runs.

  SetThreadDescription is purely cosmetic - Qt uses it to label worker threads for debuggers
  (qthread_win.cpp). This script rewrites that one import-by-name entry in Qt6Core.dll to point
  at kernel32!GetThreadId instead: same first argument (the thread HANDLE, in RCX), Qt ignores
  the return value, and on x64 the calling convention is uniform so the ignored extra argument
  is harmless. The net effect on every OS is that Qt's thread-naming silently no-ops; nothing
  else changes. GetThreadId exists on every Windows since Vista, so the import now resolves on
  Win8 and the core loads.

  This patches a *copy* of the redistributable Qt6Core.dll that ships in the bundle; it does not
  touch the fork's own sources or the deskflow-core.exe build. Run it as a packaging step on the
  Qt6Core.dll staged next to deskflow-core.exe. It is idempotent.

  Usage:  powershell -NoProfile -ExecutionPolicy Bypass -File patch-qt6core-win8.ps1 <path\to\Qt6Core.dll>

  Keep this file PURE ASCII: Windows PowerShell 5.1 mangles em-dashes/arrows in source.
#>
param(
  [Parameter(Mandatory=$true)][string]$Dll
)
$ErrorActionPreference = "Stop"

if (-not (Test-Path $Dll)) { throw "not found: $Dll" }

$bytes = [System.IO.File]::ReadAllBytes($Dll)

# The import-by-name entry is { WORD Hint; CHAR Name[]; } - a null-terminated ASCII name.
$target = "SetThreadDescription"
$repl   = "GetThreadId"           # 11 chars, fits inside the 20-char original
$tb = [System.Text.Encoding]::ASCII.GetBytes($target)

# Locate "SetThreadDescription\0" (require the NUL so we match the import string, not a substring).
function Find-Name([byte[]]$buf, [byte[]]$needle) {
  $hits = @()
  for ($i = 0; $i -le ($buf.Length - $needle.Length - 1); $i++) {
    $ok = $true
    for ($j = 0; $j -lt $needle.Length; $j++) { if ($buf[$i+$j] -ne $needle[$j]) { $ok = $false; break } }
    if ($ok -and $buf[$i+$needle.Length] -eq 0) { $hits += $i }
  }
  return $hits
}

$hits = @(Find-Name $bytes $tb)
if ($hits.Count -eq 0) {
  if (@(Find-Name $bytes ([System.Text.Encoding]::ASCII.GetBytes($repl))).Count -gt 0) {
    Write-Output "[patch-qt6core-win8] already patched (no SetThreadDescription import found): $Dll"
    exit 0
  }
  throw "SetThreadDescription import not found in $Dll (wrong file or unexpected Qt build)"
}
if ($hits.Count -gt 1) { Write-Output "[patch-qt6core-win8] WARNING: $($hits.Count) occurrences, patching all" }

foreach ($off in $hits) {
  # Zero the 2-byte Hint preceding the name so a stale hint index cannot mis-bind; the loader
  # then resolves purely by the (new) name.
  $bytes[$off-2] = 0; $bytes[$off-1] = 0
  $rb = [System.Text.Encoding]::ASCII.GetBytes($repl)
  for ($k = 0; $k -lt $tb.Length; $k++) {
    if ($k -lt $rb.Length) { $bytes[$off+$k] = $rb[$k] } else { $bytes[$off+$k] = 0 }
  }
}

[System.IO.File]::WriteAllBytes($Dll, $bytes)
Write-Output "[patch-qt6core-win8] patched $($hits.Count) import(s): SetThreadDescription to GetThreadId in $Dll"

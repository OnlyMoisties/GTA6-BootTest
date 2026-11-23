# Build, run under the harness, and print a filtered summary. Usage: go.ps1 [-Timeout 40] [-Tail 60] [-StubTail 60]
param([string]$Game = $env:GTA6_GAME_EXE, [int]$Timeout = 40, [int]$Tail = 60, [int]$StubTail = 60, [switch]$NoBuild, [string]$Flags = "")
# -Flags takes harness options as one space-separated string, e.g. -Flags "-skip -bp=2478E27,5C5FF00 -maxhits=4"
$X = @($Flags -split '\s+' | Where-Object { $_ })
$root = $PSScriptRoot
if (-not $Game) { "Pass -Game <path to game_scarlett_master.exe> or set GTA6_GAME_EXE"; return }
if (-not $NoBuild) { $b = cmd /c "$root\src\build.bat" 2>&1; if ($b -notmatch 'BUILD OK') { $b; return } }
$out = & "$root\bin\harness.exe" $Game "$root\bin\stubs" "$root\logs" $Timeout run @X
"=== harness (filtered, last $Tail) ==="
$out | Where-Object { $_ -notmatch '^DLL ' -and $_ -notmatch '0x40080201|0xE0000001' -and $_ -notmatch 'rip=0x7FF[EF]' } | Select-Object -Last $Tail
"=== stubs.log ==="
if (Test-Path "$root\logs\stubs.log") { $s = Get-Content "$root\logs\stubs.log"; "lines: $($s.Count)"; $s | Select-Object -Last $StubTail } else { "none" }

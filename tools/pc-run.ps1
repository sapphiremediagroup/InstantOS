# Run commands in InstantOS on this PC without touching it: writes them to
# AUTORUN.SH on the INSTANTOS partition, then restarts into InstantOS once
# (tools/reboot-to-instantos.ps1, firmware BootNext). There login renames the
# script to /autorun.old (so it runs on that boot only), waits for the desktop
# and runs it with bash; the script ends in `reboot`, and since BootNext is
# one-shot the PC comes back up in Windows. The script's output is in
# AUTORUN.LOG, the kernel log in KERNEL.LOG, both on the INSTANTOS partition.
#
#   tools\pc-run.ps1 'gl-window 600 > /GLWIN.TXT'
#   tools\pc-run.ps1 -NoReboot 'gpu-test all > /GPU.TXT'   # only write the script

param(
    [switch]$NoReboot,
    [Parameter(Mandatory = $true, ValueFromRemainingArguments = $true)][string[]]$Commands
)
$ErrorActionPreference = 'Stop'

$volume = Get-Volume -FileSystemLabel 'INSTANTOS' -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $volume -or -not $volume.DriveLetter) {
    Write-Host 'No INSTANTOS partition with a drive letter. Run tools/install-esp-windows.ps1 first.'
    exit 1
}
$root = "$($volume.DriveLetter):\"

$lines = @(
    '# written by tools/pc-run.ps1'
    'export PATH=/bin'
    'exec > /autorun.log 2>&1'
    'echo "autorun: start"'
) + $Commands + @(
    'echo "autorun: done, restarting"'
    'reboot'
)
# bash wants LF line endings and no BOM.
[System.IO.File]::WriteAllText("${root}AUTORUN.SH", (($lines -join "`n") + "`n"), (New-Object System.Text.UTF8Encoding($false)))
Remove-Item "${root}AUTORUN.LOG" -ErrorAction SilentlyContinue
Write-Host "Wrote ${root}AUTORUN.SH:"
Get-Content "${root}AUTORUN.SH" | ForEach-Object { Write-Host "  $_" }

if (-not $NoReboot) {
    & "$PSScriptRoot\reboot-to-instantos.ps1"
}

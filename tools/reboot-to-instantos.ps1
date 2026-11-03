# Restart this PC straight into InstantOS once (the next boot after that is
# the normal order again). Finds the firmware entry whose path is
# \EFI\InstantOS\iboot.efi and sets it as the firmware's one-time BootNext.
# Double-click or run from PowerShell; it asks for Administrator rights.

$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    Start-Process powershell -Verb RunAs -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$PSCommandPath`""
    exit
}

$id = $null
$current = $null
foreach ($line in (bcdedit /enum firmware)) {
    if ($line -match '^identifier\s+(\{[0-9a-fA-F-]+\})') { $current = $Matches[1] }
    elseif ($line -match '^path\s+\\EFI\\InstantOS\\iboot\.efi' -and $current) { $id = $current; break }
}
if (-not $id) {
    Write-Host 'No firmware entry boots \EFI\InstantOS\iboot.efi. Run tools/install-esp-windows.ps1 first.'
    Read-Host 'Press Enter to close'
    exit 1
}

bcdedit /set '{fwbootmgr}' bootsequence $id
if ($LASTEXITCODE -ne 0) { Read-Host 'bcdedit failed; press Enter to close'; exit 1 }
Write-Host "Next boot: InstantOS ($id). Restarting..."
shutdown /r /t 0

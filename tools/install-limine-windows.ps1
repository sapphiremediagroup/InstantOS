# Makes Limine this PC's boot menu (InstantOS, Windows, Arch Linux), run as
# Administrator after the build (cmake --build build --target iso), with
# Limine's BOOTX64.EFI (its v10.x-binary branch) in build\limine:
#
#   powershell -ExecutionPolicy Bypass -File tools\install-limine-windows.ps1
#
# 1. Finds the InstantOS partition: the GPT type 9566fa50-... (after
#    instant-install) or the FAT32 volume labelled INSTANTOS (before). Its GPT
#    partition GUID does not change when instant-install reformats it, so the
#    iboot.cfg written here stays right.
# 2. Backs up the whole EFI system partition to D:\InstantOS-ESP-backup\<time>
#    (bcdedit /export for the locked boot store).
# 3. Copies InstantOS's boot files as install-esp-windows.ps1 does
#    (\EFI\InstantOS\iboot.efi, \EFI\BOOT\INSTANTOS.EFI and INITRD) and writes
#    \EFI\InstantOS\iboot.cfg: root=<that partition's GUID>.
# 4. Puts Limine in \EFI\Limine (BOOTX64.EFI + limine.conf from
#    tools\boot\limine.conf, with the path of the arch-linux*.efi found on the
#    partition; that entry is commented out when there is none).
# 5. Adds a "Limine" entry to the firmware boot menu and, if you say so, puts it
#    first. Windows Boot Manager stays in the menu: the firmware's boot menu
#    (F-key at power-on) or `bcdedit /set {fwbootmgr} displayorder {bootmgr}
#    /addfirst` brings it back as the default.
# Nothing else on the EFI partition is touched. Asks before each change.
# Everything it does is logged to D:\InstantOS-limine.log.

param(
    [string]$BackupDrive = "D"
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$boot = Join-Path $root "build\iso\EFI\BOOT"
$limine = Join-Path $root "build\limine\BOOTX64.EFI"
$initrd = Join-Path $boot "INITRD"
$pcInitrd = Join-Path $root "build\pc\INITRD"   # with the toolchain (tools/make-pc-initrd.sh)
$template = Join-Path $root "tools\boot\limine.conf"
$instantOSType = "{9566fa50-7665-4f44-962e-113fd99cdc87}"
$log = "$($BackupDrive):\InstantOS-limine.log"
Start-Transcript -Path $log -Force | Out-Null

function Ask($question) {
    $answer = Read-Host "$question Type YES to go ahead"
    return $answer -eq "YES"
}

try {
    $admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $admin) { throw "Run this as Administrator." }
    foreach ($file in "BOOTX64.EFI", "INSTANTOS.EFI", "INITRD") {
        if (-not (Test-Path (Join-Path $boot $file))) { throw "Missing $boot\${file} (build the iso target first)." }
    }
    if ((Test-Path $pcInitrd) -and (Get-Item $pcInitrd).LastWriteTime -ge (Get-Item $initrd).LastWriteTime) {
        $initrd = $pcInitrd
        Write-Host ("INITRD: build\pc\INITRD, {0:N0} MB, with the C/C++ toolchain" -f ((Get-Item $initrd).Length / 1MB))
    } else {
        Write-Host "INITRD: the ISO's, WITHOUT the toolchain (build\pc\INITRD is missing or older: tools/make-pc-initrd.sh)"
    }
    if (-not (Test-Path $limine)) { throw "Missing $limine (Limine's BOOTX64.EFI from its v10.x-binary branch)." }

    # ---- 1. the InstantOS partition ----------------------------------------------
    $part = Get-Partition | Where-Object { $_.GptType -eq $instantOSType } | Select-Object -First 1
    if (-not $part) {
        $volume = Get-Volume | Where-Object { $_.FileSystemLabel -eq "INSTANTOS" } | Select-Object -First 1
        if ($volume) { $part = Get-Partition -DriveLetter $volume.DriveLetter }
    }
    if (-not $part) { throw "No InstantOS partition (GPT type $instantOSType, or a FAT32 volume labelled INSTANTOS)." }
    $guid = $part.Guid.Trim("{}").ToLower()
    Write-Host ("InstantOS partition: disk {0} partition {1}, {2:N1} GB, GUID {3}" -f
        $part.DiskNumber, $part.PartitionNumber, ($part.Size / 1GB), $guid)

    # ---- 2. the EFI system partition -----------------------------------------------
    $letter = (68..90 | ForEach-Object { [char]$_ } | Where-Object { -not (Test-Path "$($_):\") } | Select-Object -Last 1)
    mountvol "$($letter):" /S
    if ($LASTEXITCODE -ne 0) { throw "mountvol could not mount the EFI system partition." }
    $esp = "$($letter):"
    try {
        Write-Host "EFI system partition on $esp; it holds:"
        Get-ChildItem "$esp\EFI" | Format-Table Name, LastWriteTime -AutoSize | Out-String | Write-Host
        $backup = "$($BackupDrive):\InstantOS-ESP-backup\$(Get-Date -Format yyyyMMdd-HHmmss)"
        robocopy "$esp\" $backup /E /COPY:DT /R:1 /W:1 /NFL /NDL /NJH /NP /XF BCD BCD.LOG BCD.LOG1 BCD.LOG2 | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Backing up the EFI partition failed (robocopy $LASTEXITCODE)." }
        bcdedit /export "$backup\BCD-export" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "bcdedit /export failed." }
        Write-Host "Backed up to $backup"

        $windows = Test-Path "$esp\EFI\Microsoft\Boot\bootmgfw.efi"
        $arch = Get-ChildItem "$esp\EFI" -Recurse -Filter "arch-linux*.efi" -ErrorAction SilentlyContinue | Select-Object -First 1
        $archPath = if ($arch) { "/" + $arch.FullName.Substring($esp.Length + 1).Replace("\", "/") } else { $null }
        Write-Host ("Windows Boot Manager: {0}; Arch Linux: {1}" -f
            $(if ($windows) { "found" } else { "NOT found" }), $(if ($archPath) { $archPath } else { "no arch-linux*.efi found" }))

        # ---- 3. + 4. the files ------------------------------------------------------
        $conf = Get-Content $template -Raw
        if ($archPath) {
            $conf = $conf.Replace("boot():/EFI/Linux/arch-linux.efi", "boot():$archPath")
        } else {
            $conf = $conf -replace "(?ms)^/Arch Linux.*?(?=^/|\z)", ""
        }
        Write-Host "limine.conf will be:"
        Write-Host $conf
        # Room for the new INITRD besides what is already there.
        $oldInitrd = Get-Item "$esp\EFI\BOOT\INITRD" -ErrorAction SilentlyContinue
        $room = ([IO.DriveInfo]"$($letter):\").AvailableFreeSpace +$(if ($oldInitrd) { $oldInitrd.Length } else { 0 })
        $need = (Get-Item $initrd).Length + 8MB
        if ($room -lt $need) {
            throw ("The EFI partition has {0:N0} MB for the INITRD, which needs {1:N0} MB. Nothing copied." -f ($room / 1MB), ($need / 1MB))
        }
        if (-not (Ask "Copy InstantOS's boot files, write iboot.cfg (root=$guid) and put Limine in \EFI\Limine?")) {
            throw "Stopped: nothing copied."
        }
        New-Item -ItemType Directory -Force "$esp\EFI\InstantOS", "$esp\EFI\BOOT", "$esp\EFI\Limine" | Out-Null
        Copy-Item (Join-Path $boot "BOOTX64.EFI") "$esp\EFI\InstantOS\iboot.efi" -Force
        Copy-Item (Join-Path $boot "INSTANTOS.EFI") "$esp\EFI\BOOT\INSTANTOS.EFI" -Force
        Copy-Item $initrd "$esp\EFI\BOOT\INITRD" -Force
        if ((Get-Item "$esp\EFI\BOOT\INITRD").Length -ne (Get-Item $initrd).Length) { throw "INITRD was not copied whole." }
        Set-Content -Path "$esp\EFI\InstantOS\iboot.cfg" -Value "# InstantOS boot settings (tools\install-limine-windows.ps1)`nroot=$guid" -Encoding ascii
        Copy-Item $limine "$esp\EFI\Limine\BOOTX64.EFI" -Force
        Set-Content -Path "$esp\EFI\Limine\limine.conf" -Value $conf -Encoding ascii -NoNewline
        Get-ChildItem "$esp\EFI\InstantOS", "$esp\EFI\Limine" | Format-Table FullName, Length -AutoSize | Out-String | Write-Host
    } finally {
        mountvol $esp /D | Out-Null
    }

    # ---- 5. the firmware boot menu -------------------------------------------------
    # Entries are blocks separated by blank lines; take the identifier from the
    # Limine block itself (a regex over the whole listing matched the first
    # block's, {fwbootmgr}, so "make Limine first" did nothing on a rerun).
    $id = $null
    $block = @()
    foreach ($line in @(bcdedit /enum firmware) + @("")) {
        if ($line.Trim() -eq "") {
            if (($block -match "^description\s+Limine\s*$") -and -not $id) {
                $m = [regex]::Match(($block -join "`n"), "identifier\s+(\{[0-9a-zA-Z-]+\})")
                if ($m.Success) { $id = $m.Groups[1].Value }
            }
            $block = @()
        } else {
            $block += $line
        }
    }
    if ($id) {
        Write-Host "The firmware boot menu already has a Limine entry $id."
    } elseif (Ask "Add 'Limine' to the firmware boot menu (bcdedit)?") {
        $copy = bcdedit /copy "{bootmgr}" /d "Limine"
        $id = [regex]::Match($copy, "\{[0-9a-fA-F-]+\}").Value
        if (-not $id) { throw "bcdedit /copy failed: $copy" }
        bcdedit /set $id path "\EFI\Limine\BOOTX64.EFI" | Out-Null
        bcdedit /set "{fwbootmgr}" displayorder $id /addlast | Out-Null
        Write-Host "Added boot entry $id (remove: bcdedit /delete $id)."
    }
    if ($id -and (Ask "Make Limine the default (first in the firmware boot order)?")) {
        bcdedit /set "{fwbootmgr}" displayorder $id /addfirst | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "bcdedit could not put $id first." }
        Write-Host "Limine is first. Back to Windows first: bcdedit /set {fwbootmgr} displayorder {bootmgr} /addfirst"
    }
    Write-Host "Firmware boot order now (first boots by default):"
    bcdedit /enum "{fwbootmgr}" | Select-String -Pattern "displayorder|^\s+\{" | ForEach-Object { Write-Host "  $_" }
    Write-Host "Done. Restart (not Shut down): Limine's menu has InstantOS, Windows and Arch Linux."
} catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
} finally {
    Stop-Transcript | Out-Null
    Read-Host "Press Enter to close"
}

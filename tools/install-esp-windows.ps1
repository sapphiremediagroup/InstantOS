# Installs the built InstantOS boot files on this PC from Windows (the
# counterpart of cp2esp.sh), run as Administrator:
#
#   powershell -ExecutionPolicy Bypass -File tools\install-esp-windows.ps1
#
# 1. Makes the partition InstantOS writes to (its kernel log, /etc): D: is
#    shrunk by 2 GB and the space formatted FAT32 with the label INSTANTOS.
#    InstantOS mounts only such a partition on a GPT disk (see
#    gptFindInstantOSPartition); it never mounts the EFI partition, Windows or
#    Linux. Skipped when an INSTANTOS volume exists. Asks first.
# 2. Backs up the whole EFI system partition to D:\InstantOS-ESP-backup\<time>.
# 3. Copies only InstantOS's files: \EFI\InstantOS\iboot.efi (the loader) and
#    \EFI\BOOT\INSTANTOS.EFI + \EFI\BOOT\INITRD (where iboot looks for them).
#    Nothing else on the partition is touched; \EFI\BOOT\BOOTX64.EFI is left alone.
# 4. Adds an "InstantOS" entry to the firmware's boot menu (bcdedit). Asks first.
#    Remove it with: bcdedit /delete {id-it-prints}
#
# Everything it does is logged to D:\InstantOS-install.log.

param(
    [string]$ShrinkDrive = "D",
    [int]$PartitionMB = 2048
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$boot = Join-Path $root "build\iso\EFI\BOOT"
$log = "$($ShrinkDrive):\InstantOS-install.log"
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
        if (-not (Test-Path (Join-Path $boot $file))) { throw "Missing $boot\${file} (build the ISO first)." }
    }

    # ---- 1. the INSTANTOS partition --------------------------------------------
    $volume = Get-Volume | Where-Object { $_.FileSystemLabel -eq "INSTANTOS" }
    if ($volume) {
        Write-Host "INSTANTOS volume already there ($($volume.DriveLetter):), keeping it."
    } else {
        $part = Get-Partition -DriveLetter $ShrinkDrive
        $disk = Get-Disk -Number $part.DiskNumber
        if ($disk.PartitionStyle -ne "GPT") { throw "Disk $($disk.Number) is not GPT." }
        $supported = Get-PartitionSupportedSize -DriveLetter $ShrinkDrive
        $newSize = $part.Size - ($PartitionMB * 1MB)
        if ($newSize -lt $supported.SizeMin) { throw "$($ShrinkDrive): cannot shrink by $PartitionMB MB." }
        Write-Host ("Disk {0} ({1}): {2}: is {3:N1} GB; it becomes {4:N1} GB and a {5} MB FAT32 partition labelled INSTANTOS is made after it." -f
            $disk.Number, $disk.FriendlyName, $ShrinkDrive, ($part.Size / 1GB), ($newSize / 1GB), $PartitionMB)
        if (-not (Ask "Shrink $($ShrinkDrive): and make the partition?")) { throw "Stopped: no partition made." }
        Resize-Partition -DriveLetter $ShrinkDrive -Size $newSize
        $new = New-Partition -DiskNumber $disk.Number -Size ($PartitionMB * 1MB) -AssignDriveLetter
        $volume = Format-Volume -Partition $new -FileSystem FAT32 -NewFileSystemLabel "INSTANTOS" -Confirm:$false
        Write-Host "Made INSTANTOS ($($volume.DriveLetter):)."
    }

    # ---- 2. + 3. the EFI system partition ---------------------------------------
    $letter = (68..90 | ForEach-Object { [char]$_ } | Where-Object { -not (Test-Path "$($_):\") } | Select-Object -Last 1)
    mountvol "$($letter):" /S
    if ($LASTEXITCODE -ne 0) { throw "mountvol could not mount the EFI system partition." }
    $esp = "$($letter):"
    try {
        Write-Host "EFI system partition on $esp; it holds:"
        Get-ChildItem "$esp\EFI" | Format-Table Name, LastWriteTime -AutoSize | Out-String | Write-Host
        $backup = "$($ShrinkDrive):\InstantOS-ESP-backup\$(Get-Date -Format yyyyMMdd-HHmmss)"
        # Windows keeps its live boot store (\EFI\Microsoft\Boot\BCD and its logs)
        # locked; it is saved with bcdedit /export instead.
        robocopy "$esp\" $backup /E /COPY:DT /R:1 /W:1 /NFL /NDL /NJH /NP /XF BCD BCD.LOG BCD.LOG1 BCD.LOG2 | Out-Null
        if ($LASTEXITCODE -ge 8) { throw "Backing up the EFI partition failed (robocopy $LASTEXITCODE)." }
        bcdedit /export "$backup\BCD-export" | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "bcdedit /export failed." }
        Write-Host "Backed up to $backup"

        New-Item -ItemType Directory -Force "$esp\EFI\InstantOS" | Out-Null
        New-Item -ItemType Directory -Force "$esp\EFI\BOOT" | Out-Null
        Copy-Item (Join-Path $boot "BOOTX64.EFI") "$esp\EFI\InstantOS\iboot.efi" -Force
        Copy-Item (Join-Path $boot "INSTANTOS.EFI") "$esp\EFI\BOOT\INSTANTOS.EFI" -Force
        Copy-Item (Join-Path $boot "INITRD") "$esp\EFI\BOOT\INITRD" -Force
        Write-Host "Copied iboot.efi, INSTANTOS.EFI and INITRD."
        Get-ChildItem "$esp\EFI\InstantOS", "$esp\EFI\BOOT" | Format-Table FullName, Length -AutoSize | Out-String | Write-Host
    } finally {
        mountvol $esp /D | Out-Null
    }

    # ---- 4. the boot menu entry --------------------------------------------------
    $existing = bcdedit /enum firmware | Select-String -Pattern "InstantOS"
    if ($existing) {
        Write-Host "The firmware boot menu already has an InstantOS entry."
    } elseif (Ask "Add 'InstantOS' to the firmware boot menu (bcdedit)?") {
        $copy = bcdedit /copy "{bootmgr}" /d "InstantOS"
        $id = [regex]::Match($copy, "\{[0-9a-fA-F-]+\}").Value
        if (-not $id) { throw "bcdedit /copy failed: $copy" }
        bcdedit /set $id path "\EFI\InstantOS\iboot.efi" | Out-Null
        bcdedit /set "{fwbootmgr}" displayorder $id /addlast | Out-Null
        Write-Host "Added boot entry $id (remove: bcdedit /delete $id)."
    }

    try {
        $secure = Confirm-SecureBootUEFI
        Write-Host ("Secure Boot is {0}." -f ($(if ($secure) { "ON: turn it off in the firmware setup, iboot is not signed" } else { "off" })))
    } catch {
        Write-Host "Secure Boot state unknown."
    }
    Write-Host "Done. Boot InstantOS: Restart (not Shut down) and pick InstantOS in the boot menu."
} catch {
    Write-Host "ERROR: $_" -ForegroundColor Red
} finally {
    Stop-Transcript | Out-Null
    Read-Host "Press Enter to close"
}

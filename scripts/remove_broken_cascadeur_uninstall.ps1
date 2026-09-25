# Run as Administrator to remove a broken Cascadeur Chinese uninstall entry.
# This is only needed when the uninstaller file has already been deleted but the
# "Installed Apps" entry still points to the missing .inno\uninsNNN.exe.

$ErrorActionPreference = 'Stop'

$key = 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\CascadeurChinese.Inno.Release_is1'

if (Test-Path $key) {
    Remove-Item -Path $key -Recurse -Force
    Write-Output "Removed uninstall registry entry: $key"
} else {
    Write-Output "Uninstall registry entry already absent: $key"
}

# Clean any leftover shortcuts that point to a removed ChineseLauncher folder.
$shortcuts = @(
    "$env:PUBLIC\Desktop\Cascadeur 中文版.lnk",
    "$env:ProgramData\Microsoft\Windows\Start Menu\Programs\Cascadeur 中文版.lnk",
    "$env:APPDATA\Microsoft\Windows\Start Menu\Programs\Cascadeur 中文版.lnk"
)
foreach ($shortcut in $shortcuts) {
    if (Test-Path $shortcut) {
        Remove-Item -Path $shortcut -Force
        Write-Output "Removed leftover shortcut: $shortcut"
    }
}

Write-Output 'Done. The broken Cascadeur Chinese entry should no longer appear in Installed Apps.'

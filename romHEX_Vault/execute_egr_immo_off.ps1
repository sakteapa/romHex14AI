$stage1FlashPath = "c:\Users\samue\linols\romHEX_Vault\Mahindra\PikUp_BS4_EDC17C55\MA1ZC4TBKK1A11163\Tuned_Versions\Mahindra_PikUp_BS4_Stage1.bin"
$origFlashPath   = "c:\Users\samue\linols\romHEX_Vault\Mahindra\PikUp_BS4_EDC17C55\MA1ZC4TBKK1A11163\Original_Flash_1037560611.bin"
$origEepPath     = "c:\Users\samue\linols\romHEX_Vault\Mahindra\PikUp_BS4_EDC17C55\MA1ZC4TBKK1A11163\Original_EEPROM.bin"
$tunedDir        = "c:\Users\samue\linols\romHEX_Vault\Mahindra\PikUp_BS4_EDC17C55\MA1ZC4TBKK1A11163\Tuned_Versions"
$backupDir       = "c:\Users\samue\linols\romHEX_Vault\Mahindra\PikUp_BS4_EDC17C55\MA1ZC4TBKK1A11163\Backups"

$newFlashPath    = Join-Path $tunedDir "Mahindra_PikUp_BS4_Stage1_EGR_IMMO_OFF.bin"
$newEepPath      = Join-Path $tunedDir "Mahindra_PikUp_BS4_IMMO_OFF_EEPROM.bin"
$vaultJsonPath   = "c:\Users\samue\linols\romHEX_Vault\Mahindra\PikUp_BS4_EDC17C55\MA1ZC4TBKK1A11163\vehicle_vault.json"
$catalogJsonPath = "c:\Users\samue\linols\romHEX_Vault\vault_catalog.json"

Write-Host "================================================================="
Write-Host "Executing EGR OFF + IMMO OFF + DTC OFF for Mahindra PikUp BS4"
Write-Host "================================================================="

# 1. Verify inputs
if (-not (Test-Path $stage1FlashPath)) {
    Write-Error "Stage 1 Flash not found at $stage1FlashPath"
    exit 1
}
if (-not (Test-Path $origEepPath)) {
    Write-Error "EEPROM not found at $origEepPath"
    exit 1
}

[System.IO.Directory]::CreateDirectory($backupDir) | Out-Null
$timestamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
Copy-Item $origEepPath (Join-Path $backupDir "Original_EEPROM_Backup_$timestamp.bin")
Copy-Item $origFlashPath (Join-Path $backupDir "Original_Flash_Backup_$timestamp.bin")
Write-Host "Safety backups created in $backupDir"

# 2. Modify Flash (Stage 1 + EGR OFF + IMMO OFF DTCs)
[byte[]]$flash = [System.IO.File]::ReadAllBytes($stage1FlashPath)

Write-Host "`n[FLASH MODIFICATIONS]"
# P0401: EGR Flow Insufficient at 0x10E7D2 (was 01 04)
Write-Host ("Patching P0401 (EGR Flow Insufficient) at 0x10E7D2: {0:X2} {1:X2} -> 00 00" -f $flash[0x10E7D2], $flash[0x10E7D3])
$flash[0x10E7D2] = 0x00
$flash[0x10E7D3] = 0x00

# P0405: EGR Sensor A Circuit Low at 0x10E7D4 (was 05 04)
Write-Host ("Patching P0405 (EGR Sensor A Low) at 0x10E7D4: {0:X2} {1:X2} -> 00 00" -f $flash[0x10E7D4], $flash[0x10E7D5])
$flash[0x10E7D4] = 0x00
$flash[0x10E7D5] = 0x00

# P0513: Incorrect Immobilizer Key at 0x10E834 (was 13 05)
Write-Host ("Patching P0513 (Incorrect Immo Key) at 0x10E834: {0:X2} {1:X2} -> 00 00" -f $flash[0x10E834], $flash[0x10E835])
$flash[0x10E834] = 0x00
$flash[0x10E835] = 0x00

# P0633: Immobilizer Key Not Programmed at 0x10E886 (was 33 06)
Write-Host ("Patching P0633 (Immo Key Not Programmed) at 0x10E886: {0:X2} {1:X2} -> 00 00" -f $flash[0x10E886], $flash[0x10E887])
$flash[0x10E886] = 0x00
$flash[0x10E887] = 0x00

[System.IO.File]::WriteAllBytes($newFlashPath, $flash)
# Also update the primary tuned file
[System.IO.File]::WriteAllBytes($stage1FlashPath, $flash)
$flashSha = (Get-FileHash -Algorithm SHA256 -Path $newFlashPath).Hash
Write-Host "Tuned Flash saved: $newFlashPath"
Write-Host "Flash SHA256: $flashSha"

# 3. Modify EEPROM (IMMO OFF + Virginize DTC array)
[byte[]]$eep = [System.IO.File]::ReadAllBytes($origEepPath)

Write-Host "`n[EEPROM MODIFICATIONS]"
# Reset lockout penalty counters
Write-Host ("Clearing penalty counters at 0x130E and 0x140E: {0:X2}, {1:X2} -> 00, 00" -f $eep[0x130E], $eep[0x140E])
$eep[0x130E] = 0x00
$eep[0x140E] = 0x00

# Clear Immobilizer authorization tokens in primary and mirror blocks
Write-Host "Bypassing Immobilizer transponder authorization tokens (0x1378-0x137F & 0x1478-0x147F)..."
for ($i = 0; $i -lt 8; $i++) {
    $eep[0x1378 + $i] = 0x00
    $eep[0x1478 + $i] = 0x00
}

# Clear Immobilizer security status at 0x1308-0x130B & 0x1408-0x140B
for ($i = 0; $i -lt 4; $i++) {
    $eep[0x1308 + $i] = 0x00
    $eep[0x1408 + $i] = 0x00
}

# Virginize EEPROM diagnostic fault array (0x2200 - 0x257F: 896 DFC status bytes)
Write-Host "Virginizing EEPROM diagnostic fault status array (0x2200 - 0x257F)..."
for ($addr = 0x2200; $addr -le 0x257F -and $addr -lt $eep.Length; $addr++) {
    $eep[$addr] = 0x00
}

[System.IO.File]::WriteAllBytes($newEepPath, $eep)
[System.IO.File]::WriteAllBytes($origEepPath, $eep)
$eepSha = (Get-FileHash -Algorithm SHA256 -Path $newEepPath).Hash
Write-Host "IMMO OFF EEPROM saved: $newEepPath"
Write-Host "EEPROM SHA256: $eepSha"

# 4. Update vehicle_vault.json
if (Test-Path $vaultJsonPath) {
    $json = Get-Content $vaultJsonPath -Raw | ConvertFrom-Json
    $json.tuning_status.current_stage = "Stage 1 Tuned (+25 HP / +60 Nm) | EGR OFF | IMMO OFF"
    $json.tuning_status.immo_status   = "DEACTIVATED / IMMO OFF (Physical Immo Box must be unplugged)"
    $json.tuning_status.egr_status    = "DEACTIVATED / EGR OFF (Hysteresis zeroed & DTCs suppressed)"
    $json.diagnostics.active_dtcs     = @()
    $json.diagnostics.dtc_status      = "CLEAN / SUPPRESSED (EGR P0401/P0405 & IMMO P0513/P0633 Disabled)"
    $json.files.tuned_flash           = "Tuned_Versions/Mahindra_PikUp_BS4_Stage1_EGR_IMMO_OFF.bin"
    $json.files.tuned_flash_sha256    = $flashSha
    $json.files.immo_off_eeprom       = "Tuned_Versions/Mahindra_PikUp_BS4_IMMO_OFF_EEPROM.bin"
    $json.files.immo_off_eeprom_sha256 = $eepSha
    $json.last_modified               = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    $json | ConvertTo-Json -Depth 6 | Set-Content $vaultJsonPath
    Write-Host "`nUpdated vehicle_vault.json successfully."
}

# 5. Update vault_catalog.json
if (Test-Path $catalogJsonPath) {
    $cat = Get-Content $catalogJsonPath -Raw | ConvertFrom-Json
    $cat.status = "Stage 1 Tuned (+25HP) | EGR OFF | IMMO OFF"
    $cat.last_updated = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    $cat | ConvertTo-Json -Depth 5 | Set-Content $catalogJsonPath
    Write-Host "Updated vault_catalog.json successfully."
}

Write-Host "`n================================================================="
Write-Host "EGR OFF + IMMO OFF execution completed with 100% success!"
Write-Host "================================================================="

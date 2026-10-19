<#
.SYNOPSIS
    romHEX14 Vehicle Vault & Export Database Manager
.DESCRIPTION
    Fast CLI tool to search, inspect, and retrieve tuned ECU firmware packages,
    checksum hashes, and bilingual calibration PDF reports.
.EXAMPLE
    .\vault_manager.ps1 -List
    .\vault_manager.ps1 -Search "EGR"
    .\vault_manager.ps1 -Info "03_Stage1_EGR_IMMO_OFF"
    .\vault_manager.ps1 -Open "03_Stage1_EGR_IMMO_OFF"
#>
param(
    [switch]$List,
    [string]$Search,
    [string]$Info,
    [string]$Open,
    [string]$VIN
)

$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$dbPath = Join-Path $scriptDir "Export\vault_database.json"
if (-not (Test-Path $dbPath)) {
    $dbPath = Join-Path $scriptDir "vault_database.json"
}

if (-not (Test-Path $dbPath)) {
    Write-Error "Database not found at $dbPath"
    exit 1
}

$records = Get-Content $dbPath -Raw | ConvertFrom-Json

function Show-RecordTable($items) {
    Write-Host "`n" ("=" * 95) -ForegroundColor Cyan
    Write-Host "  ROMHEX14 VEHICLE VAULT & EXPORT INDEX" -ForegroundColor Cyan
    Write-Host ("=" * 95) -ForegroundColor Cyan
    $table = $items | Select-Object @{N="Package ID";E={$_.package_id}},
                                    @{N="Vehicle";E={$_.vehicle_model}},
                                    @{N="Power (HP/Nm)";E={"$($_.power_hp) HP / $($_.torque_nm) Nm"}},
                                    @{N="EGR";E={if($_.egr_status -match "DEACTIVATED"){"OFF"}else{"STOCK"}}},
                                    @{N="IMMO";E={if($_.immo_status -match "BYPASS"){"OFF"}else{"STOCK"}}},
                                    @{N="Folder";E={$_.folder_relative}}
    $table | Format-Table -AutoSize
}

# 1. Info Command
if ($Info) {
    $matched = $records | Where-Object { $_.package_id -like "*$Info*" -or $_.folder_relative -like "*$Info*" }
    if (-not $matched) {
        Write-Warning "No package found matching '$Info'."
        exit 0
    }
    foreach ($m in $matched) {
        Write-Host "`n" ("=" * 70) -ForegroundColor Green
        Write-Host "PACKAGE: $($m.package_id) - $($m.package_title_en)" -ForegroundColor Green
        Write-Host "MIZO:    $($m.package_title_mz)" -ForegroundColor Gray
        Write-Host ("=" * 70) -ForegroundColor Green
        Write-Host "Vehicle:       $($m.vehicle_model) (VIN: $($m.vin))"
        Write-Host "Engine & ECU:  $($m.engine) | $($m.ecu_type) ($($m.software_id))"
        Write-Host "Output:        $($m.power_hp) HP / $($m.torque_nm) Nm (Fuel Limit: $($m.fuel_limit_mg_hub) mg/hub)"
        Write-Host "Boost / Rail:  Peak $($m.boost_peak_mbar) mbar / $($m.rail_peak_bar) bar"
        Write-Host "EGR Status:    $($m.egr_status)"
        Write-Host "IMMO Status:   $($m.immo_status)"
        Write-Host "DTC Status:    $($m.dtc_status)"
        Write-Host "`nFILES:"
        Write-Host "  Flash:  $($m.flash_file)"
        Write-Host "  SHA256: $($m.flash_sha256)"
        if ($m.eeprom_file) {
            Write-Host "  EEPROM: $($m.eeprom_file)"
            Write-Host "  SHA256: $($m.eeprom_sha256)"
        }
        Write-Host "  Report: $($m.pdf_report)"
        Write-Host "`nDirectory:     $($m.folder_relative)"
        Write-Host "Checksum Protocol: $($m.checksum_protocol)"
    }
    exit 0
}

# 2. Open in Explorer
if ($Open) {
    $matched = $records | Where-Object { $_.package_id -like "*$Open*" } | Select-Object -First 1
    if ($matched) {
        $abs = Join-Path (Split-Path $dbPath -Parent) (Split-Path $matched.folder_relative -Leaf)
        if (-not (Test-Path $abs)) {
            $abs = Join-Path $scriptDir $matched.folder_relative
        }
        Write-Host "Opening folder: $abs" -ForegroundColor Green
        Invoke-Item $abs
    } else {
        Write-Warning "Could not find package '$Open'."
    }
    exit 0
}

# 3. Search Command
if ($Search) {
    $s = $Search.ToLower()
    $matched = $records | Where-Object {
        $_.package_id.ToLower().Contains($s) -or
        $_.vehicle_model.ToLower().Contains($s) -or
        $_.package_title_en.ToLower().Contains($s) -or
        $_.package_title_mz.ToLower().Contains($s) -or
        $_.egr_status.ToLower().Contains($s) -or
        $_.immo_status.ToLower().Contains($s) -or
        $_.vin.ToLower().Contains($s) -or
        $_.software_id.ToLower().Contains($s)
    }
    Write-Host "Found $($matched.Count) results matching '$Search':" -ForegroundColor Yellow
    Show-RecordTable $matched
    exit 0
}

# 4. Search by VIN
if ($VIN) {
    $matched = $records | Where-Object { $_.vin -like "*$VIN*" }
    Write-Host "Found $($matched.Count) results for VIN '$VIN':" -ForegroundColor Yellow
    Show-RecordTable $matched
    exit 0
}

# Default / List
Show-RecordTable $records
Write-Host "Usage Tips:" -ForegroundColor Gray
Write-Host "  .\vault_manager.ps1 -Search 'EGR'           (Search by keyword)" -ForegroundColor Gray
Write-Host "  .\vault_manager.ps1 -Info '03'              (Show full technical specs & hashes)" -ForegroundColor Gray
Write-Host "  .\vault_manager.ps1 -Open '03'              (Open package folder in Explorer)" -ForegroundColor Gray

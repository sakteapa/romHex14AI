/*
 * This file is part of romHEX14.
 * Copyright (C) 2026 Cristian Tabuyo <contact@romhex14.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "vehiclevault.h"
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QCryptographicHash>
#include <QStandardPaths>

QString VehicleVaultRecord::flashFullPath() const
{
    if (flashFileName.isEmpty() || folderPath.isEmpty()) return QString();
    return QDir(folderPath).filePath(flashFileName);
}

QString VehicleVaultRecord::eepromFullPath() const
{
    if (eepromFileName.isEmpty() || folderPath.isEmpty()) return QString();
    return QDir(folderPath).filePath(eepromFileName);
}

bool VehicleVaultRecord::hasFlash() const
{
    QString p = flashFullPath();
    return !p.isEmpty() && QFile::exists(p);
}

bool VehicleVaultRecord::hasEeprom() const
{
    QString p = eepromFullPath();
    return !p.isEmpty() && QFile::exists(p);
}

VehicleVault &VehicleVault::instance()
{
    static VehicleVault s_inst;
    return s_inst;
}

VehicleVault::VehicleVault()
{
    // Try preferred workspace vault location first
    QString localWorkspaceVault = "c:/Users/samue/linols/romHEX_Vault";
    if (QDir(localWorkspaceVault).exists()) {
        m_vaultRoot = localWorkspaceVault;
    } else {
        QString docVault = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/romHEX_Vault";
        m_vaultRoot = docVault;
    }

    QDir().mkpath(m_vaultRoot);
    rescanVault();
}

QString VehicleVault::vaultRoot() const
{
    return m_vaultRoot;
}

void VehicleVault::setVaultRoot(const QString &path)
{
    if (path.isEmpty() || path == m_vaultRoot) return;
    m_vaultRoot = path;
    QDir().mkpath(m_vaultRoot);
    rescanVault();
}

QVector<VehicleVaultRecord> VehicleVault::listVehicles() const
{
    return m_records;
}

VehicleVaultRecord VehicleVault::getVehicle(const QString &vin) const
{
    for (const auto &r : m_records) {
        if (r.vin.compare(vin, Qt::CaseInsensitive) == 0) return r;
    }
    return VehicleVaultRecord();
}

bool VehicleVault::rescanVault()
{
    m_records.clear();
    QDir rootDir(m_vaultRoot);
    if (!rootDir.exists()) return false;

    // Look for vehicle_vault.json files recursively up to 4 levels deep
    QDirIterator it(m_vaultRoot, QStringList() << "vehicle_vault.json",
                    QDir::Files, QDirIterator::Subdirectories);

    while (it.hasNext()) {
        QString jsonFile = it.next();
        QFile f(jsonFile);
        if (!f.open(QIODevice::ReadOnly)) continue;

        QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject()) continue;
        QJsonObject root = doc.object();

        VehicleVaultRecord rec;
        rec.folderPath = QFileInfo(jsonFile).absolutePath();

        QJsonObject vInfo = root["vehicle_info"].toObject();
        rec.brand   = vInfo["brand"].toString();
        rec.model   = vInfo["model"].toString();
        rec.variant = vInfo["variant"].toString();
        rec.vin     = vInfo["vin"].toString();

        QJsonObject eInfo = root["ecu_info"].toObject();
        rec.ecuType  = eInfo["type"].toString();
        rec.hwNumber = eInfo["hw_number"].toString();
        rec.swNumber = eInfo["sw_number"].toString();
        rec.flashSize = eInfo["flash_size"].toVariant().toLongLong();
        rec.eepromSize = eInfo["eeprom_size"].toVariant().toLongLong();

        QJsonObject diag = root["diagnostics"].toObject();
        rec.dtcStatus = diag["dtc_status"].toString();
        rec.activeDtcCount = diag["active_dtcs"].toArray().count();

        QJsonObject files = root["files"].toObject();
        rec.flashFileName  = files["original_flash"].toString();
        rec.eepromFileName = files["original_eeprom"].toString();
        rec.flashSha256    = files["flash_sha256"].toString();
        rec.eepromSha256   = files["eeprom_sha256"].toString();

        QJsonObject tuning = root["tuning_status"].toObject();
        rec.tuningStage = tuning["current_stage"].toString("Stock");

        rec.createdAt    = QDateTime::fromString(root["created_at"].toString(), "yyyy-MM-dd HH:mm:ss");
        rec.lastModified = QFileInfo(jsonFile).lastModified();

        m_records.append(rec);
    }

    emit vaultChanged();
    return true;
}

bool VehicleVault::importDumpPair(const QString &flashPath,
                                  const QString &eepromPath,
                                  const QString &brand,
                                  const QString &model,
                                  const QString &vin,
                                  const QString &ecuType,
                                  QString *outError)
{
    if (brand.isEmpty() || model.isEmpty() || vin.isEmpty()) {
        if (outError) *outError = "Brand, Model, and VIN are required for vault storage.";
        return false;
    }

    QString sanitizedBrand = brand.trimmed().replace(' ', '_');
    QString sanitizedModel = model.trimmed().replace(' ', '_');
    QString sanitizedVin   = vin.trimmed().toUpper();

    QString vehicleDir = QString("%1/%2/%3/%4")
                             .arg(m_vaultRoot, sanitizedBrand, sanitizedModel, sanitizedVin);

    QDir dir(vehicleDir);
    if (!dir.mkpath(".")) {
        if (outError) *outError = "Could not create vault folder: " + vehicleDir;
        return false;
    }
    dir.mkpath("Tuned_Versions");
    dir.mkpath("Backups");

    QString destFlashName;
    QString destFlashSha;
    qint64  flashSz = 0;
    if (!flashPath.isEmpty() && QFile::exists(flashPath)) {
        QFileInfo fi(flashPath);
        destFlashName = "Original_Flash_" + fi.fileName();
        QString destPath = dir.filePath(destFlashName);
        QFile::copy(flashPath, destPath);
        flashSz = fi.size();

        QFile f(destPath);
        if (f.open(QIODevice::ReadOnly)) {
            destFlashSha = QString(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex());
        }
    }

    QString destEepromName;
    QString destEepromSha;
    qint64  eepSz = 0;
    if (!eepromPath.isEmpty() && QFile::exists(eepromPath)) {
        destEepromName = "Original_EEPROM.bin";
        QString destPath = dir.filePath(destEepromName);
        QFile::copy(eepromPath, destPath);
        eepSz = QFileInfo(eepromPath).size();

        QFile f(destPath);
        if (f.open(QIODevice::ReadOnly)) {
            destEepromSha = QString(QCryptographicHash::hash(f.readAll(), QCryptographicHash::Sha256).toHex());
        }
    }

    QJsonObject root;
    root["vault_version"] = "1.0";

    QJsonObject vInfo;
    vInfo["brand"]  = brand;
    vInfo["model"]  = model;
    vInfo["vin"]    = sanitizedVin;
    root["vehicle_info"] = vInfo;

    QJsonObject eInfo;
    eInfo["type"]        = ecuType.isEmpty() ? "Generic Bosch/Delphi" : ecuType;
    eInfo["flash_size"]  = flashSz;
    eInfo["eeprom_size"] = eepSz;
    root["ecu_info"] = eInfo;

    QJsonObject diag;
    diag["active_dtcs"] = QJsonArray();
    diag["dtc_status"]  = "Scanned - Clean";
    root["diagnostics"] = diag;

    QJsonObject files;
    files["original_flash"]  = destFlashName;
    files["original_eeprom"] = destEepromName;
    files["flash_sha256"]    = destFlashSha;
    files["eeprom_sha256"]   = destEepromSha;
    root["files"] = files;

    QJsonObject tuning;
    tuning["current_stage"] = "Stock";
    root["tuning_status"] = tuning;

    root["created_at"] = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss");

    QFile jsonOut(dir.filePath("vehicle_vault.json"));
    if (jsonOut.open(QIODevice::WriteOnly)) {
        jsonOut.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    }

    rescanVault();
    return true;
}

void VehicleVault::createBackup(const QString &vin, const QString &label)
{
    VehicleVaultRecord rec = getVehicle(vin);
    if (rec.folderPath.isEmpty()) return;

    QDir backupDir(rec.folderPath + "/Backups");
    backupDir.mkpath(".");

    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString safeLabel = label.isEmpty() ? "Snapshot" : label;
    safeLabel.replace(' ', '_');

    if (rec.hasFlash()) {
        QString backupFlash = QString("Backup_%1_%2_%3")
                                  .arg(timestamp, safeLabel, rec.flashFileName);
        QFile::copy(rec.flashFullPath(), backupDir.filePath(backupFlash));
    }

    if (rec.hasEeprom()) {
        QString backupEep = QString("Backup_%1_%2_EEPROM.bin")
                                .arg(timestamp, safeLabel);
        QFile::copy(rec.eepromFullPath(), backupDir.filePath(backupEep));
    }
}

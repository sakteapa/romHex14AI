/*
 * This file is part of romHEX14.
 * Copyright (C) 2026 Cristian Tabuyo <contact@romhex14.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <QVector>
#include <QDateTime>
#include <QJsonObject>

struct VehicleVaultRecord {
    QString   brand;
    QString   model;
    QString   variant;
    QString   vin;
    QString   ecuType;
    QString   hwNumber;
    QString   swNumber;
    QString   folderPath;
    QString   flashFileName;
    QString   eepromFileName;
    qint64    flashSize = 0;
    qint64    eepromSize = 0;
    QString   flashSha256;
    QString   eepromSha256;
    int       activeDtcCount = 0;
    QString   dtcStatus;
    QString   tuningStage = "Stock";
    QDateTime createdAt;
    QDateTime lastModified;

    QString flashFullPath() const;
    QString eepromFullPath() const;
    bool hasFlash() const;
    bool hasEeprom() const;
};

class VehicleVault : public QObject {
    Q_OBJECT
public:
    static VehicleVault &instance();

    QString vaultRoot() const;
    void setVaultRoot(const QString &path);

    QVector<VehicleVaultRecord> listVehicles() const;
    VehicleVaultRecord getVehicle(const QString &vin) const;

    bool importDumpPair(const QString &flashPath,
                        const QString &eepromPath,
                        const QString &brand,
                        const QString &model,
                        const QString &vin,
                        const QString &ecuType = QString(),
                        QString *outError = nullptr);

    bool rescanVault();
    void createBackup(const QString &vin, const QString &label);

signals:
    void vaultChanged();

private:
    VehicleVault();
    void loadCatalog();
    void saveCatalog();
    void scanDirectories();

    QString m_vaultRoot;
    QVector<VehicleVaultRecord> m_records;
};

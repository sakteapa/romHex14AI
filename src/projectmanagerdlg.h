/*
 * This file is part of romHEX14.
 * Copyright (C) 2026 Cristian Tabuyo <contact@romhex14.com>
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDialog>
#include <QTableWidget>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QComboBox>
#include <QTabWidget>

class QCloseEvent;
class QTreeWidget;
class QTreeWidgetItem;

namespace winols { class Config; class WinOlsCatalogReader; }

class ProjectManagerDialog : public QDialog {
    Q_OBJECT

public:
    explicit ProjectManagerDialog(QWidget *parent = nullptr);
    ~ProjectManagerDialog() override;

    QString selectedPath() const { return m_selectedPath; }

protected:
    void closeEvent(QCloseEvent *event) override;

signals:
    void openProjectRequested(const QString &path);
    void newProjectRequested();
    /// Emitted when the user picks an entry from the WinOLS catalog
    /// tab and we successfully resolved its on-disk path.  MainWindow
    /// is expected to import it via the OLS pipeline.
    void openOlsRequested(const QString &path);

    /// Sprint I — user clicked "Build similarity index" in the WinOLS
    /// Catalog tab.  MainWindow opens the BuildIndexProgressDlg with
    /// the same auto-config logic it uses for its own Find menu entry.
    void buildSimilarityIndexRequested();

    /// Vehicle Vault: open a raw Flash/ROM file directly in the editor
    void openRomRequested(const QString &path);

private slots:
    void onOpen();
    void onRename();
    void onRemove();
    void onDeleteFile();
    void onShowInExplorer();
    void onDoubleClick(int row, int col);
    void onSelectionChanged();
    void onFilterChanged(const QString &text);
    void refresh();

    // ── Vehicle Vault tab ──
    void onVaultFilterChanged(const QString &text);
    void onVaultOpen();
    void onVaultShowInExplorer();
    void onVaultImportPair();
    void onVaultRefresh();
    void onVaultSelectionChanged();
    void onVaultDoubleClick(int row, int col);

    // ── WinOLS tab ──
    void onCatalogChanged(int idx);
    void onCatalogFilterChanged(const QString &text);
    void onCatalogActivated(QTreeWidgetItem *item, int column);
    void onCatalogConfigure();
    void onCatalogRefresh();

private:
    QWidget *buildVaultTab();
    QWidget *buildLocalTab();
    QWidget *buildCatalogTab();
    void     loadCatalogList();
    void     loadCatalog(const QString &dbPath);
    void     applyCatalogFilter(const QString &needle);

    void     buildTable(const QString &filter = {});
    void     buildVaultTable(const QString &filter = {});
    QString  pathAt(int row) const;
    QString  vaultFlashPathAt(int row) const;
    QString  vaultFolderPathAt(int row) const;

    // Vehicle Vault tab
    QTableWidget *m_vaultTable       = nullptr;
    QLineEdit    *m_vaultFilterEdit  = nullptr;
    QLabel       *m_vaultCountLbl    = nullptr;
    QPushButton  *m_vaultOpenBtn     = nullptr;
    QPushButton  *m_vaultExplorerBtn = nullptr;
    QPushButton  *m_vaultImportBtn   = nullptr;
    QPushButton  *m_vaultRefreshBtn  = nullptr;

    // Local projects tab
    QTableWidget *m_table       = nullptr;
    QLineEdit    *m_filterEdit  = nullptr;
    QPushButton  *m_openBtn     = nullptr;
    QPushButton  *m_renameBtn   = nullptr;
    QPushButton  *m_removeBtn   = nullptr;
    QPushButton  *m_deleteBtn   = nullptr;
    QLabel       *m_countLbl    = nullptr;

    // WinOLS catalog tab
    QComboBox    *m_catCombo    = nullptr;
    QLineEdit    *m_catFilter   = nullptr;
    QTreeWidget  *m_catTree     = nullptr;
    QLabel       *m_catStatus   = nullptr;
    QPushButton  *m_catOpenBtn  = nullptr;
    QPushButton  *m_catCfgBtn   = nullptr;
    QPushButton  *m_catRefresh  = nullptr;

    QTabWidget   *m_tabs        = nullptr;
    QString       m_selectedPath;
    winols::Config *m_winCfg    = nullptr;
};

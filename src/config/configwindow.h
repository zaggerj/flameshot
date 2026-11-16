// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#pragma once

#include <QTabWidget>

class FileNameEditor;
class ShortcutsWidget;
class GeneralConf;
class QFileSystemWatcher;
class VisualsEditor;
class QWidget;

class ConfigWindow : public QWidget
{
    Q_OBJECT
public:
    explicit ConfigWindow(QWidget* parent = nullptr);

    // Used when the UI language is switched at runtime: the window is
    // re-created so all hand-built labels pick up the new translation, and
    // the active tab / position are restored afterwards.
    int currentTab() const { return m_tabWidget->currentIndex(); }
    void setCurrentTab(int index)
    {
        if (index >= 0 && index < m_tabWidget->count()) {
            m_tabWidget->setCurrentIndex(index);
        }
    }

signals:
    void updateChildren();

protected:
    void keyPressEvent(QKeyEvent*);

private:
    QTabWidget* m_tabWidget;

    FileNameEditor* m_filenameEditor;
    QWidget* m_filenameEditorTab;

    ShortcutsWidget* m_shortcuts;
    QWidget* m_shortcutsTab;

    GeneralConf* m_generalConfig;
    QWidget* m_generalConfigTab;

    VisualsEditor* m_visuals;
    QWidget* m_visualsTab;

    void initErrorIndicator(QWidget* tab, QWidget* widget);
};

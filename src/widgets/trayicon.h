#include <QSystemTrayIcon>
#include <QByteArray>
#include <QPair>
#include <QVector>

#pragma once

class QAction;

class TrayIcon : public QSystemTrayIcon
{
    Q_OBJECT
public:
    TrayIcon(QObject* parent = nullptr);
    virtual ~TrayIcon();

#if !defined(DISABLE_UPDATE_CHECKER)
    QAction* appUpdates();
#endif

private:
    void initTrayIcon();
    void initMenu();
    void retranslateMenu();
    void initScreenMenu();
    void updateCaptureActionShortcut();
#if !defined(DISABLE_UPDATE_CHECKER)
    void updateCheckUpdatesMenuVisibility();
#endif

    void startGuiCapture();
    void startGuiCaptureOnScreen(int screenIndex);

    // Source texts of every tr()-based menu action, so a runtime language
    // switch can re-run tr() without rebuilding the menu (and without losing
    // the signal connections).
    QVector<QPair<QAction*, QByteArray>> m_trActions;
    QMenu* m_menu;
    QMenu* m_screenMenu;
    QAction* m_captureAction;
    QAction* m_scrollCaptureAction;
    QAction* m_launcherAction;
    QAction* m_infoAction;
#if !defined(DISABLE_UPDATE_CHECKER)
    QAction* m_appUpdates;
#endif
};

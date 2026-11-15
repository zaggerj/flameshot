// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2017-2019 Alejandro Sirgo Rica & Contributors

#pragma once

#include <QAbstractNativeEventFilter>
#include <QKeySequence>
#include <QObject>
#include <QVector>

// Windows-only global hotkeys. On Windows every global action is registered
// here, including TAKE_SCREENSHOT (default Ctrl+Alt+A) - QHotkey is only
// created on macOS. All of them are read from ConfigHandler, may be cleared
// to disable them, and can be re-registered without restarting.
class GlobalShortcutFilter
  : public QObject
  , public QAbstractNativeEventFilter
{
    Q_OBJECT
public:
    explicit GlobalShortcutFilter(QObject* parent = nullptr);
    ~GlobalShortcutFilter() override;

    bool nativeEventFilter(const QByteArray& eventType,
                           void* message,
                           qintptr* result) override;

public slots:
    // Re-read the configuration and re-register every global hotkey.
    void reloadShortcuts();

signals:
    void captureRequested();
    void historyRequested();
    void scrollCaptureRequested();

private:
    struct Binding
    {
        int id;
    };

    void unregisterAll();
    bool registerBinding(int id, const QKeySequence& sequence);

    QVector<Binding> m_bindings;
};

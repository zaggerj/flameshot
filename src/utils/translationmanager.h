// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 zagger fork contributors
#pragma once

#include <QObject>
#include <QString>
#include <QTranslator>

// Owns the two QTranslator objects for the whole process.
//
// Upstream installed translators on stack objects in main() at startup only,
// so the "UI language" combo box had to ask for a restart. This class keeps
// them alive and reloads them on demand, then emits languageChanged() so the
// config window and tray menu can re-create their strings in place.
class TranslationManager : public QObject
{
    Q_OBJECT
public:
    static TranslationManager* instance();

    // Called once from main() with the language stored in the config.
    void initialize();

    // Runtime switch: saves the setting, reloads both translators and
    // notifies every window. No restart required.
    void applyLanguage(const QString& code);

    QString currentLanguage() const;

signals:
    void languageChanged();

private:
    explicit TranslationManager(QObject* parent = nullptr);
    Q_DISABLE_COPY(TranslationManager)

    void loadTranslators(const QString& code);

    QTranslator m_appTranslator;
    QTranslator m_qtTranslator;
    bool m_installed = false;
};

// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 zagger fork contributors

#include "utils/translationmanager.h"
#include "utils/confighandler.h"
#include "utils/pathinfo.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QLibraryInfo>
#include <QLocale>
#include <QWidget>

TranslationManager* TranslationManager::instance()
{
    static TranslationManager self;
    return &self;
}

TranslationManager::TranslationManager(QObject* parent)
  : QObject(parent)
{}

QString TranslationManager::currentLanguage() const
{
    return ConfigHandler().uiLanguage();
}

void TranslationManager::initialize()
{
    loadTranslators(ConfigHandler().uiLanguage());
}

void TranslationManager::applyLanguage(const QString& code)
{
    if (code.isEmpty() || code == ConfigHandler().uiLanguage()) {
        return;
    }

    ConfigHandler().setUiLanguage(code);
    loadTranslators(code);

    // Qt's own widgets (dialog button boxes, standard buttons, ...) refresh
    // their text on QEvent::LanguageChange. Hand-built windows are rebuilt by
    // the languageChanged() listeners (Flameshot::onLanguageChanged,
    // TrayIcon::retranslateMenu).
    emit languageChanged();
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        QEvent event(QEvent::LanguageChange);
        QCoreApplication::sendEvent(widget, &event);
    }
}

void TranslationManager::loadTranslators(const QString& code)
{
    if (m_installed) {
        qApp->removeTranslator(&m_appTranslator);
        qApp->removeTranslator(&m_qtTranslator);
        m_installed = false;
    }

    const bool automatic = (code == QStringLiteral("auto"));
    bool found = false;

    // Application catalogue: first match among all deployment locations.
    for (const QString& path : PathInfo::translationsPaths()) {
        if (automatic) {
            // Load the language detected from the system.
            found = m_appTranslator.load(QLocale(),
                                         QStringLiteral("Internationalization"),
                                         QStringLiteral("_"),
                                         path);
            if (!found) {
                // Neither the exact locale nor the bare language is shipped
                // (it_CH with only it_IT available): take any translation of
                // the same language rather than falling back to English.
                const QString language = QLocale().name().section('_', 0, 0);
                const QStringList sameLanguage = QDir(path).entryList(
                  { QStringLiteral("Internationalization_%1_*.qm").arg(language) },
                  QDir::Files);
                found = !sameLanguage.isEmpty() &&
                        m_appTranslator.load(sameLanguage.constFirst(), path);
            }
        } else {
            found = m_appTranslator.load(
              QStringLiteral("Internationalization_") + code, path);
        }
        if (found) {
            break;
        }
    }
    if (!found) {
        if (automatic) {
            qWarning() << QStringLiteral("No Flameshot translation found for %1")
                            .arg(QLocale().uiLanguages().join(", "));
        } else {
            qWarning() << QStringLiteral("No Flameshot translation found for %1")
                            .arg(code);
        }
    }

    // Qt base catalogue (standard dialogs etc.).
    bool foundQt = false;
    const QString qtPath = QLibraryInfo::path(QLibraryInfo::TranslationsPath);
    if (automatic) {
        foundQt = m_qtTranslator.load(
          QLocale::system(), QStringLiteral("qt"), QStringLiteral("_"), qtPath);
    } else {
        foundQt = m_qtTranslator.load(QStringLiteral("qt_") + code, qtPath);
    }
    if (!foundQt && automatic) {
        qWarning() << QStringLiteral("No Qt translation found for %1")
                        .arg(QLocale::languageToString(
                          QLocale::system().language()));
    }

    qApp->installTranslator(&m_appTranslator);
    qApp->installTranslator(&m_qtTranslator);
    m_installed = true;
}

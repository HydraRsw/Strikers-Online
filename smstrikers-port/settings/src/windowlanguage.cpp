#include "windowlanguage.h"

#include <QCoreApplication>
#include <QLibraryInfo>
#include <QLocale>
#include <QSettings>
#include <QTranslator>

// Set by CMake to the languages actually compiled in, "" for an English-only build.
#ifndef STRIKERS_SETTINGS_TRANSLATIONS
#define STRIKERS_SETTINGS_TRANSLATIONS ""
#endif

namespace {

const QString kKey = QStringLiteral("window/language");

} // namespace

QStringList WindowLanguage::available()
{
    return QString::fromLatin1(STRIKERS_SETTINGS_TRANSLATIONS)
        .split(QLatin1Char(','), Qt::SkipEmptyParts);
}

QString WindowLanguage::chosen()
{
    return QSettings().value(kKey).toString();
}

void WindowLanguage::choose(const QString& tag)
{
    QSettings settings;
    if (tag.isEmpty())
        settings.remove(kKey);
    else
        settings.setValue(kKey, tag);
}

QString WindowLanguage::resolve(const QString& tag)
{
    const QLocale preferred = tag.isEmpty() ? QLocale::system() : QLocale(tag);
    // English has no catalogue, so QTranslator::load(QLocale, ) would skip to the next language.
    for (const QString& ui : preferred.uiLanguages())
    {
        const QLocale::Language language = QLocale(ui).language();
        if (language == QLocale::English)
            return QString();
        for (const QString& t : available())
        {
            if (QLocale(t).language() == language)
                return t;
        }
    }
    return QString();
}

QString WindowLanguage::name(const QString& tag)
{
    // nativeLanguageName() can name the region too: "American English", "español de España".
    switch (tag.isEmpty() ? QLocale::English : QLocale(tag).language())
    {
    case QLocale::English: return QStringLiteral("English");
    case QLocale::German: return QStringLiteral("Deutsch");
    case QLocale::Spanish: return QStringLiteral("Español");
    case QLocale::French: return QStringLiteral("Français");
    default: return QLocale(tag).nativeLanguageName();
    }
}

void WindowLanguage::install(const QString& forced)
{
    static QTranslator* qt = nullptr;
    static QTranslator* mine = nullptr;
    for (QTranslator** t : { &qt, &mine })
    {
        if (*t != nullptr)
        {
            QCoreApplication::removeTranslator(*t);
            delete *t;
            *t = nullptr;
        }
    }

    const QString tag = resolve(forced.isEmpty() ? chosen() : forced);
    if (tag.isEmpty())
        return;

    // Qt's own strings from the installation, behind the copy merged into ours.
    const QLocale locale(tag);
    qt = new QTranslator(qApp);
    if (qt->load(locale, QStringLiteral("qtbase"), QStringLiteral("_"),
                 QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
        QCoreApplication::installTranslator(qt);

    mine = new QTranslator(qApp);
    if (mine->load(QStringLiteral(":/i18n/strikers-settings_") + tag))
        QCoreApplication::installTranslator(mine);
}

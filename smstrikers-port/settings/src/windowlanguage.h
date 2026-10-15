// The language this window is written in. It is not the game's language.

#pragma once

#include <QString>
#include <QStringList>

namespace WindowLanguage {

// The catalogues the build embedded, as tags ("de"); English is the source and has none.
QStringList available();

// The player's pick from the window, "" to follow the system.
QString chosen();
void choose(const QString& tag);

// The catalogue `tag` comes out as, "" for English; an empty `tag` asks the system.
QString resolve(const QString& tag);

// The language's own name for itself: "Deutsch", "Français".
QString name(const QString& tag);

// Replaces the installed catalogues with those for `forced`, else the pick, else the system's.
void install(const QString& forced);

} // namespace WindowLanguage

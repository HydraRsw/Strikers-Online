#pragma once

#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>

namespace DiscImage {

struct Info
{
    bool ok = false;
    QString error;
    bool notThisGame = false;
    QString gameId;
    QString title;
    QString format;
    bool nkit = false;
    int files = 0;
    qint64 bytes = 0;
};

Info inspect(const QString& path);

QStringList nameFilters();

QString defaultExtractDir(const QString& gameId);

QString extractedGameId(const QString& files);

enum class Result
{
    Ok,
    Cancelled,
    Failed,
};

using Progress = std::function<void(qint64 done, qint64 total)>;

Result extract(const QString& image, const QString& outDir, const Progress& progress,
               const std::atomic<bool>& cancel, QString* error);

} // namespace DiscImage

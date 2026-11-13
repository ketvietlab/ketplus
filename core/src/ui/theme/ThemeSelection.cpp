#include "ui/theme/ThemeSelection.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <limits>

namespace ketplus {
namespace {
struct ReadSelection {
    ThemeSelection selection;
    quint64 revision{0};
    bool exists{false};
    bool recoverable{false};
    QString error;
};
bool validId(const QString& id) {
    static const QRegularExpression format(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,127}\\z"));
    return format.match(id).hasMatch();
}
bool validSelection(const ThemeSelection& selection) {
    return validId(selection.schemeId) && validId(selection.lightSchemeId) &&
           validId(selection.darkSchemeId);
}
ReadSelection readSelection(const QString& path) {
    ReadSelection result;
    QFile file(path);
    result.exists = file.exists();
    if (QFileInfo(path).isSymLink()) {
        result.exists = true;
        result.error = QStringLiteral("Theme selection cannot be a symlink");
        return result;
    }
    if (!result.exists)
        return result;
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Cannot read theme selection");
        return result;
    }
    constexpr qsizetype maximumBytes = 16 * 1024;
    const auto bytes = file.read(maximumBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        result.error = QStringLiteral("Cannot read theme selection");
        return result;
    }
    // An oversized file may contain a future schema outside the read budget.
    if (bytes.size() > maximumBytes) {
        result.error = QStringLiteral("Theme selection exceeds size limit");
        return result;
    }
    const auto document = QJsonDocument::fromJson(bytes);
    if (!document.isObject()) {
        result.recoverable = true;
        result.error = QStringLiteral("Invalid theme selection JSON");
        return result;
    }
    const auto object = document.object();
    const auto version = object.value(QStringLiteral("schemaVersion"));
    if (version.isDouble() && version.toDouble() > 1) {
        result.error = QStringLiteral("Unsupported theme selection schema version");
        return result;
    }
    const auto follow = object.value(QStringLiteral("followSystem"));
    const auto revision = object.value(QStringLiteral("revision"));
    static const QRegularExpression digits(QStringLiteral("^[1-9][0-9]{0,19}\\z"));
    bool revisionOk = false;
    if (revision.isString() && digits.match(revision.toString()).hasMatch())
        result.revision = revision.toString().toULongLong(&revisionOk);
    result.selection.followSystem = follow.toBool();
    result.selection.schemeId = object.value(QStringLiteral("schemeId")).toString();
    result.selection.lightSchemeId = object.value(QStringLiteral("lightSchemeId")).toString();
    result.selection.darkSchemeId = object.value(QStringLiteral("darkSchemeId")).toString();
    if (!version.isDouble() || version.toDouble() != 1 || !follow.isBool() || !revisionOk ||
        !validSelection(result.selection)) {
        result.recoverable = true;
        result.error = QStringLiteral("Invalid theme selection fields");
    }
    return result;
}
QByteArray writeSelection(const ThemeSelection& selection, quint64 revision) {
    return QJsonDocument(QJsonObject{{QStringLiteral("schemaVersion"), 1},
                                     {QStringLiteral("revision"), QString::number(revision)},
                                     {QStringLiteral("followSystem"), selection.followSystem},
                                     {QStringLiteral("schemeId"), selection.schemeId},
                                     {QStringLiteral("lightSchemeId"), selection.lightSchemeId},
                                     {QStringLiteral("darkSchemeId"), selection.darkSchemeId}})
        .toJson();
}
} // namespace
ThemeSelectionStore::ThemeSelectionStore(QString directory, QString legacyTheme, QObject* parent)
    : QObject(parent),
      directory_(directory.isEmpty()
                     ? QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
                           .filePath(QStringLiteral("color-schemes"))
                     : std::move(directory)),
      watcher_(new QFileSystemWatcher(this)) {
    if (legacyTheme == u"light" || legacyTheme == u"dark") {
        selection_.followSystem = false;
        selection_.schemeId = legacyTheme == u"dark" ? QStringLiteral("builtin-dark")
                                                     : QStringLiteral("builtin-light");
    }
    if (!QDir().mkpath(directory_)) {
        fail(QStringLiteral("Cannot create theme selection directory"));
        return;
    }
    // Watch the directory so QSaveFile's rename never drops the subscription.
    if (!watcher_->addPath(directory_))
        fail(QStringLiteral("Cannot watch theme selection directory"));
    connect(watcher_, &QFileSystemWatcher::directoryChanged, this, &ThemeSelectionStore::refresh);
    const auto stored = readSelection(filePath());
    if (stored.exists) {
        if (!stored.error.isEmpty()) {
            fail(stored.error);
            return;
        }
        selection_ = stored.selection;
        revision_ = stored.revision;
    } else {
        // Re-read under the lock: another process may migrate first.
        // Adopt its value rather than overwriting the migration with stale settings.
        QLockFile lock(QDir(directory_).filePath(QStringLiteral(".selection.lock")));
        if (!lock.tryLock(0)) {
            fail(QStringLiteral("Theme selection is busy"));
            return;
        }
        const auto raced = readSelection(filePath());
        if (raced.exists) {
            if (!raced.error.isEmpty()) {
                fail(raced.error);
                return;
            }
            selection_ = raced.selection;
            revision_ = raced.revision;
        } else {
            QSaveFile file(filePath());
            const auto bytes = writeSelection(selection_, 1);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
                !file.commit()) {
                fail(QStringLiteral("Cannot migrate theme selection"));
                return;
            }
            revision_ = 1;
        }
    }
}
const ThemeSelection& ThemeSelectionStore::selection() const { return selection_; }
quint64 ThemeSelectionStore::revision() const { return revision_; }
QString ThemeSelectionStore::filePath() const {
    return QDir(directory_).filePath(QStringLiteral("selection.json"));
}
QString ThemeSelectionStore::lastError() const { return lastError_; }
void ThemeSelectionStore::fail(const QString& message) {
    lastError_ = message;
    emit failed(message);
}
bool ThemeSelectionStore::commit(const ThemeSelection& wanted) {
    if (!validSelection(wanted)) {
        fail(QStringLiteral("Invalid theme selection ID"));
        return false;
    }
    QLockFile lock(QDir(directory_).filePath(QStringLiteral(".selection.lock")));
    if (!lock.tryLock(0)) {
        fail(QStringLiteral("Theme selection is busy"));
        return false;
    }
    const auto stored = readSelection(filePath());
    if (!stored.error.isEmpty() && !stored.recoverable) {
        fail(stored.error);
        return false;
    }
    // Only an explicit commit repairs corrupt data, under the same lock and atomic
    // replacement as an ordinary write. Refresh/startup retain the last valid value.
    if (stored.error.isEmpty() && stored.exists && stored.selection == wanted &&
        stored.revision >= revision_) {
        const bool changedRevision = revision_ != stored.revision;
        selection_ = stored.selection;
        revision_ = stored.revision;
        lastError_.clear();
        if (changedRevision)
            emit changed();
        return true;
    }
    const quint64 previousRevision = qMax(revision_, stored.revision);
    if (previousRevision == std::numeric_limits<quint64>::max()) {
        fail(QStringLiteral("Theme selection revision exhausted"));
        return false;
    }
    const quint64 revision = previousRevision + 1;
    QSaveFile file(filePath());
    const auto bytes = writeSelection(wanted, revision);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        fail(QStringLiteral("Cannot atomically save theme selection"));
        return false;
    }
    selection_ = wanted;
    revision_ = revision;
    lastError_.clear();
    emit changed();
    return true;
}
void ThemeSelectionStore::refresh() {
    if (QDir(directory_).exists() && !watcher_->directories().contains(directory_))
        watcher_->addPath(directory_);
    const auto stored = readSelection(filePath());
    if (!stored.exists || !stored.error.isEmpty()) {
        fail(stored.error.isEmpty() ? QStringLiteral("Theme selection unavailable") : stored.error);
        return;
    }
    lastError_.clear();
    if (stored.revision <= revision_)
        return;
    selection_ = stored.selection;
    revision_ = stored.revision;
    lastError_.clear();
    emit changed();
}
} // namespace ketplus

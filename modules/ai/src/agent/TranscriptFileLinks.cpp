#include "agent/TranscriptFileLinks.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

#include <utility>

namespace ketplus {

namespace {

constexpr qint64 cacheLifetimeMs = 30'000;
constexpr int maximumTrackedFiles = 200'000;

} // namespace

void TranscriptFileLinks::setRoot(const QString& root) {
    if (root == root_)
        return;
    root_ = root;
    cache_.clear();
    files_.clear();
    filesListed_ = false;
    filesRequested_ = false;
    age_.restart();
}

void TranscriptFileLinks::setFileRequester(std::function<void()> requester) {
    requester_ = std::move(requester);
}

void TranscriptFileLinks::setFiles(const QString& root, const QStringList& entries) {
    if (root != root_)
        return;
    files_.clear();
    for (const QString& entry : entries) {
        if (!entry.endsWith(QLatin1Char('/')))
            files_.append(entry);
    }
    if (files_.size() > maximumTrackedFiles)
        files_.clear();
    filesListed_ = true;
    filesRequested_ = false;
    // Answers given before the list arrived may now resolve.
    cache_.clear();
    age_.restart();
}

void TranscriptFileLinks::requestFiles() {
    if (filesRequested_ || !requester_ || root_.isEmpty())
        return;
    filesRequested_ = true;
    requester_();
}

QString TranscriptFileLinks::href(const QString& text) {
    // A path, then an optional line (a range or column keeps its first line).
    static const QRegularExpression reference(
        QStringLiteral(R"(^(?:(?:\.\.\.|…)/)?([^\s:*?"<>|`]+?)(?::(\d+)(?:[-–]\d+)?(?::\d+)?)?$)"));
    static const QRegularExpression extension(QStringLiteral(R"(\.[A-Za-z0-9]{1,10}$)"));
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > 512 || trimmed.contains(QStringLiteral("://")))
        return {};
    const auto match = reference.match(trimmed);
    if (!match.hasMatch())
        return {};
    const QString path = match.captured(1);
    // A bare word is an identifier, not a file; it needs a folder or an extension.
    if (!path.contains(QLatin1Char('/')) && !extension.match(path).hasMatch())
        return {};
    if (!age_.isValid() || age_.elapsed() > cacheLifetimeMs) {
        // The old list keeps answering until the new one lands.
        cache_.clear();
        age_.restart();
        if (filesListed_)
            requestFiles();
    }
    const auto cached = cache_.constFind(path);
    QString resolved;
    if (cached != cache_.cend()) {
        resolved = *cached;
    } else {
        bool final = true;
        resolved = resolve(path, &final);
        if (final)
            cache_.insert(path, resolved);
    }
    if (resolved.isEmpty())
        return {};
    const QString line = match.captured(2);
    return line.isEmpty() ? resolved : resolved + QLatin1Char(':') + line;
}

QString TranscriptFileLinks::resolve(const QString& path, bool* final) {
    if (path.startsWith(QStringLiteral("~/"))) {
        const QString absolute = QDir::home().filePath(path.mid(2));
        return QFileInfo(absolute).isFile() ? absolute : QString{};
    }
    if (QDir::isAbsolutePath(path))
        return QFileInfo(path).isFile() ? path : QString{};
    if (root_.isEmpty())
        return {};
    QString relative = path;
    while (relative.startsWith(QStringLiteral("./")))
        relative = relative.mid(2);
    if (relative.startsWith(QStringLiteral("../")))
        return {};
    if (QFileInfo(QDir(root_).filePath(relative)).isFile())
        return relative;
    if (!filesListed_) {
        requestFiles();
        *final = false;
        return {};
    }
    // A path given from inside the tree opens when exactly one listed file ends with it.
    const QString suffix = QLatin1Char('/') + relative;
    QString found;
    for (const QString& file : std::as_const(files_)) {
        if (!file.endsWith(suffix))
            continue;
        if (!found.isEmpty())
            return {};
        found = file;
    }
    return found;
}

} // namespace ketplus

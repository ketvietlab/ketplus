#include "ThemeInput.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QSet>
#include <functional>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ket::theme::io {
namespace {
bool fail(Error* error, const QString& message, const QString& path) {
    if (error)
        *error = {message, path};
    return false;
}
bool relative(const QString& path) {
    return !path.isEmpty() && !path.startsWith('/') && !path.contains(':') &&
           !path.contains('\\') && !path.contains(QChar::Null);
}
} // namespace
IncludeResolver::IncludeResolver(const QString& rootDirectory)
    : root(QFileInfo(rootDirectory).canonicalFilePath()) {
    if (!QFileInfo(root).isDir())
        root.clear();
}
IncludeResolver::IncludeResolver(const ZipArchive& source) : archive(&source) {}
bool IncludeResolver::resolve(const QString& declaring, const QString& reference, QString* path,
                              Error* error) const {
    if (error)
        *error = {};
    if (path)
        path->clear();
    if (!path || !relative(reference) || (!declaring.isEmpty() && !relative(declaring)))
        return fail(error, QStringLiteral("Include must be a relative local path"), reference);
    const auto joined =
        declaring.isEmpty() ? reference : QFileInfo(declaring).path() + '/' + reference;
    const auto cleaned = QDir::cleanPath(joined).normalized(QString::NormalizationForm_C);
    if (cleaned == QStringLiteral("..") || cleaned.startsWith(QStringLiteral("../")) ||
        cleaned == QStringLiteral("."))
        return fail(error, QStringLiteral("Include escapes source root"), reference);
    if (!archive) {
        if (root.isEmpty())
            return fail(error, QStringLiteral("Source directory does not exist"), reference);
        const auto canonical = QFileInfo(QDir(root).filePath(cleaned)).canonicalFilePath();
        if (canonical.isEmpty() || !canonical.startsWith(root + '/'))
            return fail(error, QStringLiteral("Include missing or outside source root"), reference);
        // Refuse links even if their target happens to be inside the root. This
        // also keeps include identities stable for cycle detection.
        QString current = root;
        for (const auto& part : cleaned.split('/')) {
            current += '/' + part;
            const QFileInfo component(current);
            if (component.isSymLink() || component.isJunction())
                return fail(error, QStringLiteral("Include symlinks are forbidden"), reference);
        }
    }
    *path = cleaned;
    return true;
}
bool IncludeResolver::read(const QString& path, QByteArray* data, Error* error) const {
    if (error)
        *error = {};
    if (!data)
        return fail(error, QStringLiteral("Missing read output"), path);
    data->clear();
    QString normalized;
    if (!resolve({}, path, &normalized, error))
        return false;
    if (archive)
        return archive->read(normalized, data, error);
    QFile file;
#ifdef Q_OS_UNIX
    // Anchor every component to an open directory and never follow a link, even
    // if another process replaces a checked path between resolve() and open().
    int descriptor = ::open(QFile::encodeName(root).constData(),
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0)
        return fail(error, QStringLiteral("Cannot open source directory safely"), path);
    const auto parts = normalized.split('/');
    for (qsizetype i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        const int next =
            ::openat(descriptor, QFile::encodeName(parts[i]).constData(),
                     O_RDONLY | O_NOFOLLOW | O_CLOEXEC | (last ? O_NONBLOCK : O_DIRECTORY));
        ::close(descriptor);
        descriptor = next;
        if (descriptor < 0)
            return fail(error, QStringLiteral("Cannot open include safely"), path);
    }
    struct stat metadata{};
    if (::fstat(descriptor, &metadata) || !S_ISREG(metadata.st_mode) ||
        metadata.st_size > MaxFileBytes) {
        ::close(descriptor);
        return fail(error, QStringLiteral("Include must be a regular file of at most 5 MiB"), path);
    }
    if (!file.open(descriptor, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(descriptor);
        return fail(error, file.errorString(), path);
    }
#else
    file.setFileName(QDir(root).filePath(normalized));
    if (!QFileInfo(file).isFile())
        return fail(error, QStringLiteral("Include must be a regular file"), path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(error, file.errorString(), path);
    if (!QFileInfo(file).isFile() || file.size() > MaxFileBytes)
        return fail(error, QStringLiteral("Include must be a regular file of at most 5 MiB"), path);
#endif
    auto result = file.read(MaxFileBytes + 1);
    if (file.error() != QFileDevice::NoError || result.size() > MaxFileBytes)
        return fail(error, QStringLiteral("Include read failed or exceeds 5 MiB"), path);
    *data = std::move(result);
    return true;
}
bool IncludeResolver::loadJsonChain(const QString& path, QVector<JsonSource>* chain, Error* error,
                                    quint64* remainingBytes) const {
    if (error)
        *error = {};
    if (!chain)
        return fail(error, QStringLiteral("Missing include chain output"), path);
    chain->clear();
    QVector<JsonSource> result;
    QSet<QString> active;
    quint64 bytes = 0;
    std::function<bool(const QString&, int)> visit = [&](const QString& file, int level) {
        QString normalized;
        if (!resolve({}, file, &normalized, error))
            return false;
        if (level > MaxIncludeLevels)
            return fail(error, QStringLiteral("Include exceeds 16 levels"), file);
        if (active.contains(normalized))
            return fail(error, QStringLiteral("Include cycle detected"), file);
        active.insert(normalized);
        QByteArray data;
        if (!read(normalized, &data, error))
            return false;
        if (remainingBytes) {
            if (quint64(data.size()) > *remainingBytes) {
                *remainingBytes = 0;
                return fail(error, QStringLiteral("Import exceeds shared read budget"), file);
            }
            *remainingBytes -= quint64(data.size());
        }
        bytes += quint64(data.size());
        if (bytes > MaxExpandedBytes)
            return fail(error, QStringLiteral("Include chain exceeds 100 MiB"), file);
        auto document = parseJsonc(data, error);
        if (document.isNull()) {
            if (error)
                error->source = file;
            return false;
        }
        if (!document.isObject())
            return fail(error, QStringLiteral("Theme JSON root must be an object"), file);
        const auto include = document.object().value(QStringLiteral("include"));
        if (!include.isUndefined()) {
            if (!include.isString())
                return fail(error, QStringLiteral("Include must be a string"), file);
            QString parent;
            if (!resolve(normalized, include.toString(), &parent, error) ||
                !visit(parent, level + 1))
                return false;
        }
        active.remove(normalized);
        result.append({normalized, document});
        return true;
    };
    if (!visit(path, 1))
        return false;
    *chain = std::move(result);
    return true;
}
} // namespace ket::theme::io

#include "cloud/CredentialStore.h"
#include "DaemonPaths.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <optional>
#include <utility>

#if defined(Q_OS_UNIX)
#include <sys/stat.h>
#endif

namespace ketplus::daemon {
namespace {

// Credentials live in one owner-only file beside the daemon journal instead of the macOS
// keychain. Keychain items are bound to the code signature, so every rebuilt or re-signed
// helper prompted the user again for each key. This matches how CLI agents store tokens.
QString credentialPath(const QString& service) {
    const QString directory =
        QDir(remote::DaemonPaths::dataDirectory()).filePath(QStringLiteral("daemon"));
    return QDir(directory).filePath(QStringLiteral("credentials-%1.json").arg(service));
}

void setError(QString* error, const QString& message) {
    if (error != nullptr)
        *error = message;
}

bool readFile(const QString& path, QJsonObject* values, QString* error) {
    *values = {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Unable to read the daemon credential file."));
        return false;
    }
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        setError(error, QStringLiteral("The daemon credential file is corrupt."));
        return false;
    }
    *values = document.object();
    return true;
}

bool isUnsafe(const QFileInfo& info) {
#if defined(Q_OS_UNIX)
    constexpr QFileDevice::Permissions unsafe = QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                                                QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                                QFileDevice::WriteOther | QFileDevice::ExeOther;
    return info.isSymLink() || (info.permissions() & unsafe) != 0;
#else
    Q_UNUSED(info);
    return false;
#endif
}

// Only a POSIX stat identifies a file version exactly; QFileInfo's millisecond times match a
// same-size rewrite made in the same millisecond. Elsewhere the file is read every time.
std::optional<CredentialStore::FileStamp> stampOf(const QString& path) {
#if defined(Q_OS_UNIX)
    struct stat status{};
    if (::stat(QFile::encodeName(path).constData(), &status) != 0)
        return std::nullopt;
#if defined(Q_OS_DARWIN)
    const timespec modified = status.st_mtimespec;
    const timespec changed = status.st_ctimespec;
#else
    const timespec modified = status.st_mtim;
    const timespec changed = status.st_ctim;
#endif
    constexpr qint64 nanoseconds = 1'000'000'000;
    return CredentialStore::FileStamp{
        static_cast<qint64>(status.st_dev), static_cast<qint64>(status.st_ino),
        static_cast<qint64>(status.st_size), modified.tv_sec * nanoseconds + modified.tv_nsec,
        changed.tv_sec * nanoseconds + changed.tv_nsec};
#else
    Q_UNUSED(path);
    return std::nullopt;
#endif
}

bool save(const QString& path, const QJsonObject& values, QString* error) {
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        setError(error, QStringLiteral("Unable to create the daemon credential directory."));
        return false;
    }
#if defined(Q_OS_UNIX)
    QFile::setPermissions(directory,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
    QSaveFile file(path);
    const QByteArray bytes = QJsonDocument(values).toJson(QJsonDocument::Compact);
    // Restrict the temporary file before any secret is written or the file is renamed into place.
    if (!file.open(QIODevice::WriteOnly) ||
        !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner) ||
        file.write(bytes) != bytes.size() || !file.commit()) {
        setError(error, QStringLiteral("Unable to write the daemon credential file."));
        return false;
    }
    return true;
}

} // namespace

CredentialStore::CredentialStore(QString service) : service_(std::move(service)) {}

// Distinguishes "no credentials yet" (true, empty object) from an unreadable, unsafe or corrupt
// file (false). Callers must never treat the latter as missing and generate replacement keys.
// The file's safety is checked on every read; only its contents come from the snapshot, and only
// while the file is the version the snapshot was read from.
bool CredentialStore::load(QJsonObject* values, QString* error) const {
    const QString path = credentialPath(service_);
    const QFileInfo info(path);
    *values = {};
    if (!info.exists()) {
        snapshot_ = {};
        return true;
    }
    if (isUnsafe(info)) {
        snapshot_ = {};
        setError(error, QStringLiteral("The daemon credential file has unsafe permissions."));
        return false;
    }
    const std::optional<FileStamp> before = stampOf(path);
    if (snapshot_.valid && before && snapshot_.stamp == *before) {
        *values = snapshot_.values;
        return true;
    }
    snapshot_ = {};
    ++fileReads_;
    if (!readFile(path, values, error))
        return false;
    // A write between the two stats leaves the snapshot empty, and the next read parses again.
    if (before && stampOf(path) == before)
        snapshot_ = {true, *before, *values};
    return true;
}

void CredentialStore::remember(const QJsonObject& values) const {
    const std::optional<FileStamp> stamp = stampOf(credentialPath(service_));
    snapshot_ = stamp ? Snapshot{true, *stamp, values} : Snapshot{};
}

QByteArray CredentialStore::read(const QString& account, QString* error) const {
    QJsonObject values;
    if (!load(&values, error))
        return {};
    return QByteArray::fromBase64(values.value(account).toString().toLatin1());
}

bool CredentialStore::write(const QString& account, const QByteArray& value, QString* error) const {
    QJsonObject values;
    if (!load(&values, error))
        return false;
    values.insert(account, QString::fromLatin1(value.toBase64()));
    if (!save(credentialPath(service_), values, error)) {
        snapshot_ = {};
        return false;
    }
    remember(values);
    return true;
}

bool CredentialStore::remove(const QString& account, QString* error) const {
    QJsonObject values;
    if (!load(&values, error))
        return false;
    if (!values.contains(account))
        return true;
    values.remove(account);
    if (!save(credentialPath(service_), values, error)) {
        snapshot_ = {};
        return false;
    }
    remember(values);
    return true;
}

} // namespace ketplus::daemon

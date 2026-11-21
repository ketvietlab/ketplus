#include "cloud/CredentialStore.h"
#include "DaemonPaths.h"
#include "cloud/CredentialBackend.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <utility>
#if defined(Q_OS_WIN)
#include <qt_windows.h>
#endif
#if defined(Q_OS_UNIX)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace ketplus::daemon {
namespace {
constexpr qsizetype maximumRecordBytes = 16 * 1024 * 1024;
bool fail(QString* error, const QString& message) {
    if (error)
        *error = message;
    return false;
}
// A kernel lock releases on close/process exit even if permissions change during
// cleanup. Keep its empty inode; unlinking a held lock could split the lock domain.
class StoreLock {
#if defined(Q_OS_WIN)
    HANDLE handle_{INVALID_HANDLE_VALUE};
#endif
    int descriptor_{-1};

  public:
    ~StoreLock() {
#if defined(Q_OS_UNIX)
        if (descriptor_ >= 0)
            ::close(descriptor_);
#elif defined(Q_OS_WIN)
        if (handle_ != INVALID_HANDLE_VALUE)
            ::CloseHandle(handle_);
#endif
    }
    bool acquire(const QString& path, QString* error) {
#if defined(Q_OS_UNIX)
        descriptor_ = ::open(QFile::encodeName(path).constData(),
                             O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat status{};
        if (descriptor_ >= 0 && ::fstat(descriptor_, &status) == 0 && S_ISREG(status.st_mode) &&
            status.st_uid == ::geteuid() && (status.st_mode & 0077) == 0 && status.st_nlink == 1 &&
            ::flock(descriptor_, LOCK_EX | LOCK_NB) == 0)
            return true;
#elif defined(Q_OS_WIN)
        // Share mode 0: a second opener, in this or another process, is refused until this
        // handle closes, which the system also does when the process exits.
        handle_ = ::CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
                                GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle_ != INVALID_HANDLE_VALUE)
            return true;
#else
        Q_UNUSED(path);
#endif
        return fail(error, QStringLiteral(
                               "Credential storage is busy or inaccessible; retry the operation."));
    }
};
QString credentialPath(const QString& service) {
    return QDir(remote::DaemonPaths::dataDirectory())
        .filePath(QStringLiteral("daemon/credentials-%1.json").arg(service));
}
bool decode(const QJsonObject& values, QHash<QString, QByteArray>* accounts, QString* error) {
    accounts->clear();
    for (auto it = values.begin(); it != values.end(); ++it) {
        const auto value = QByteArray::fromBase64Encoding(it.value().toString().toLatin1(),
                                                          QByteArray::AbortOnBase64DecodingErrors);
        if (!it.value().isString() || !value)
            return fail(error, QStringLiteral("The credential record is corrupt."));
        accounts->insert(it.key(), value.decoded);
    }
    return true;
}
QJsonObject encode(const QHash<QString, QByteArray>& accounts) {
    QJsonObject values;
    for (auto it = accounts.begin(); it != accounts.end(); ++it)
        values.insert(it.key(), QString::fromLatin1(it.value().toBase64()));
    return values;
}
bool parse(const QByteArray& bytes, QJsonObject* object, QString* error) {
    QJsonParseError parseError;
    const auto doc = QJsonDocument::fromJson(bytes, &parseError);
    if (bytes.size() > maximumRecordBytes || parseError.error != QJsonParseError::NoError ||
        !doc.isObject())
        return fail(error, QStringLiteral("The credential record is corrupt or too large."));
    *object = doc.object();
    return true;
}
// Open before checking ownership/mode, and never follow a symlink during migration.
bool readLegacy(const QString& path, QByteArray* bytes, QString* error) {
#if defined(Q_OS_UNIX)
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return fail(error, QStringLiteral("Unable to open the legacy credential file."));
    struct stat status{};
    const bool safe = ::fstat(fd, &status) == 0 && S_ISREG(status.st_mode) &&
                      status.st_uid == ::geteuid() && (status.st_mode & 0077) == 0 &&
                      status.st_nlink == 1 && status.st_size <= maximumRecordBytes;
    QFile file;
    if (!safe || !file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return fail(error,
                    QStringLiteral(
                        "The legacy credential file has unsafe ownership, permissions or size."));
    }
    *bytes = file.read(maximumRecordBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes->size() > maximumRecordBytes)
        return fail(error, QStringLiteral("Unable to read the legacy credential file."));
    return true;
#elif defined(Q_OS_WIN)
    // No owner/mode to compare: the credential directory carries an owner-only access list.
    // A link is refused, as on POSIX, and the size is bounded before anything is read.
    const QFileInfo info(path);
    QFile file(path);
    if (info.isSymLink() || !info.isFile() || info.size() > maximumRecordBytes ||
        !file.open(QIODevice::ReadOnly))
        return fail(error,
                    QStringLiteral(
                        "The legacy credential file has unsafe ownership, permissions or size."));
    *bytes = file.read(maximumRecordBytes + 1);
    if (file.error() != QFileDevice::NoError || bytes->size() > maximumRecordBytes)
        return fail(error, QStringLiteral("Unable to read the legacy credential file."));
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(bytes);
    return fail(error, QStringLiteral("Credential migration is not supported on this platform."));
#endif
}
QString digest(const QByteArray& bytes) {
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
} // namespace

CredentialStore::CredentialStore(QString service)
    : CredentialStore(service, nativeCredentialBackend(), credentialPath(service)) {}
CredentialStore::CredentialStore(QString service, std::shared_ptr<CredentialBackend> backend,
                                 QString legacyPath)
    : service_(std::move(service)), backend_(std::move(backend)),
      legacyPath_(std::move(legacyPath)) {}

bool CredentialStore::transaction(const std::function<bool(QHash<QString, QByteArray>&)>& change,
                                  QHash<QString, QByteArray>* result, QString* error) const {
    if (error)
        error->clear();
    static const QRegularExpression safeService(QStringLiteral("^[A-Za-z0-9_.-]{1,200}$"));
    if (!backend_ || !safeService.match(service_).hasMatch())
        return fail(error, QStringLiteral("Invalid credential store configuration."));
    const QString directory = QFileInfo(legacyPath_).absolutePath();
    if (!QDir().mkpath(directory))
        return fail(error, QStringLiteral("Unable to create the credential lock directory."));
#if defined(Q_OS_UNIX)
    struct stat status{};
    if (::lstat(QFile::encodeName(directory).constData(), &status) != 0 ||
        !S_ISDIR(status.st_mode) || status.st_uid != ::geteuid() ||
        !QFile::setPermissions(directory, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                              QFileDevice::ExeOwner))
        return fail(error, QStringLiteral("Unsafe credential lock directory."));
#endif
    StoreLock lock;
    if (!lock.acquire(legacyPath_ + QStringLiteral(".lock"), error))
        return false;
    QByteArray bytes;
    const auto read = backend_->read(service_, &bytes, error);
    if (read == CredentialBackend::Result::Error)
        return false;
    QJsonObject record;
    QHash<QString, QByteArray> accounts;
    if (read == CredentialBackend::Result::Found) {
        if (!parse(bytes, &record, error))
            return false;
        if (record.value("version") != QJsonValue(1) || !record.value("accounts").isObject())
            return fail(error, QStringLiteral("Unsupported credential record format."));
        if (!decode(record.value("accounts").toObject(), &accounts, error))
            return false;
    }
    const QFileInfo legacy(legacyPath_);
    if (legacy.exists() || legacy.isSymLink()) {
        QByteArray legacyBytes;
        if (!readLegacy(legacyPath_, &legacyBytes, error))
            return false;
        if (read == CredentialBackend::Result::Missing) {
            QJsonObject values;
            if (!parse(legacyBytes, &values, error) || !decode(values, &accounts, error))
                return false;
            record = {{"version", 1}, {"accounts", values}, {"legacyDigest", digest(legacyBytes)}};
            const auto migrated = QJsonDocument(record).toJson(QJsonDocument::Compact);
            if (migrated.size() > maximumRecordBytes || !backend_->write(service_, migrated, error))
                return fail(error, QStringLiteral("Credential migration could not commit to the OS "
                                                  "store; the legacy file is retained."));
            QByteArray verified;
            if (backend_->read(service_, &verified, error) != CredentialBackend::Result::Found ||
                verified != migrated)
                return fail(
                    error,
                    QStringLiteral(
                        "Credential migration verification failed; the legacy file is retained."));
        } else if (record.value("legacyDigest").toString() != digest(legacyBytes)) {
            return fail(error,
                        QStringLiteral("Legacy credentials conflict with the OS store; preserve "
                                       "both and resolve recovery before retrying."));
        }
        // Retrying after a crash/write success never reimports stale enrollment or removed keys.
        QByteArray current;
        if (!readLegacy(legacyPath_, &current, error) || current != legacyBytes ||
            !QFile::remove(legacyPath_))
            return fail(error, QStringLiteral("Credentials are in the OS store, but legacy-file "
                                              "cleanup failed; retry before continuing."));
    }
    const auto before = accounts;
    if (change && !change(accounts))
        return false;
    if (accounts != before) {
        record.insert("version", 1);
        record.insert("accounts", encode(accounts));
        const auto updated = QJsonDocument(record).toJson(QJsonDocument::Compact);
        if (updated.size() > maximumRecordBytes)
            return fail(error, QStringLiteral("The credential record exceeds the storage limit."));
        if (!backend_->write(service_, updated, error))
            return false;
    }
    if (result)
        *result = accounts;
    return true;
}
QByteArray CredentialStore::read(const QString& account, QString* error) const {
    if (stagedAccounts_) {
        if (error)
            error->clear();
        return stagedAccounts_->value(account);
    }
    QHash<QString, QByteArray> accounts;
    if (!transaction({}, &accounts, error))
        return {};
    return accounts.value(account);
}
bool CredentialStore::write(const QString& account, const QByteArray& value, QString* error) const {
    return update([&](auto& accounts) { accounts.insert(account, value); }, error);
}
bool CredentialStore::remove(const QString& account, QString* error) const {
    return removeIf([&](const QString& candidate) { return candidate == account; }, error);
}
bool CredentialStore::removeIf(const std::function<bool(const QString&)>& match,
                               QString* error) const {
    return update(
        [&](auto& accounts) {
            accounts.removeIf([&](const auto& entry) { return match(entry.key()); });
        },
        error);
}
bool CredentialStore::update(const std::function<void(QHash<QString, QByteArray>&)>& change,
                             QString* error) const {
    if (stagedAccounts_) {
        if (error)
            error->clear();
        change(*stagedAccounts_);
        return true;
    }
    return transaction(
        [&](auto& accounts) {
            change(accounts);
            return true;
        },
        nullptr, error);
}

bool CredentialStore::batch(const std::function<bool(CredentialStore&, QString*)>& operation,
                            QString* error) const {
    QString localError;
    if (!error)
        error = &localError;
    error->clear();
    const auto apply = [&](auto& accounts) {
        CredentialStore view(service_, nullptr, {});
        view.stagedAccounts_ = &accounts;
        if (operation(view, error))
            return true;
        if (error && error->isEmpty())
            *error = QStringLiteral("Credential operation did not complete; retry.");
        return false;
    };
    if (stagedAccounts_) {
        auto nested = *stagedAccounts_;
        if (!apply(nested))
            return false;
        *stagedAccounts_ = std::move(nested);
        return true;
    }
    return transaction(apply, nullptr, error);
}
} // namespace ketplus::daemon

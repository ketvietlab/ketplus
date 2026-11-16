#include "cloud/CredentialBackend.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QSaveFile>

namespace ketplus::daemon {
namespace {
// Linked only into ketplus_fixture_daemon. Synthetic values must survive a child
// process restart without consulting a user's native credential store.
class FileCredentialBackend final : public CredentialBackend {
    QString path(const QString& service, QString* error) const {
        const QString root = qEnvironmentVariable("KETPLUS_DAEMON_DATA_DIR");
        static const QRegularExpression validService(QStringLiteral("^[A-Za-z0-9_.-]{1,200}$"));
        if (root.isEmpty() || !QDir::isAbsolutePath(root) || !QDir(root).exists() ||
            !validService.match(service).hasMatch()) {
            if (error)
                *error =
                    QStringLiteral("The fixture requires an explicit temporary data directory.");
            return {};
        }
        const QString directory = QDir(root).filePath(QStringLiteral("synthetic-credentials"));
        if (!QDir().mkpath(directory) ||
            !QFile::setPermissions(directory,
                                   QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
            if (error)
                *error = QStringLiteral("Unable to create the synthetic credential directory.");
            return {};
        }
        return QDir(directory).filePath(service + QStringLiteral(".json"));
    }

  public:
    Result read(const QString& service, QByteArray* value, QString* error) override {
        value->clear();
        const QString filePath = path(service, error);
        if (filePath.isEmpty())
            return Result::Error;
        QFile file(filePath);
        if (!file.exists())
            return Result::Missing;
        if (!file.open(QIODevice::ReadOnly)) {
            if (error)
                *error = QStringLiteral("Unable to read synthetic credentials.");
            return Result::Error;
        }
        *value = file.readAll();
        return Result::Found;
    }
    bool write(const QString& service, const QByteArray& value, QString* error) override {
        const QString filePath = path(service, error);
        if (filePath.isEmpty())
            return false;
        QSaveFile file(filePath);
        if (!file.open(QIODevice::WriteOnly) ||
            !file.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
            file.write(value) != value.size() || !file.commit()) {
            if (error)
                *error = QStringLiteral("Unable to write synthetic credentials.");
            return false;
        }
        return true;
    }
};
} // namespace
std::shared_ptr<CredentialBackend> nativeCredentialBackend() {
    return std::make_shared<FileCredentialBackend>();
}
} // namespace ketplus::daemon

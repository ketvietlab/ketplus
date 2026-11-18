#include "cloud/CredentialBackend.h"
#include <QStandardPaths>

namespace ketplus::daemon {
std::shared_ptr<CredentialBackend> platformCredentialBackend();
namespace {
class Unavailable final : public CredentialBackend {
  public:
    Result read(const QString&, QByteArray* value, QString* error) override {
        value->clear();
        if (error)
            *error = QStringLiteral("The OS credential store is unavailable in this environment.");
        return Result::Error;
    }
    bool write(const QString&, const QByteArray&, QString* error) override {
        QByteArray ignored;
        read({}, &ignored, error);
        return false;
    }
};
} // namespace
std::shared_ptr<CredentialBackend> nativeCredentialBackend() {
    // Unit tests must explicitly inject a fake or an isolated OS-store fixture.
    if (QStandardPaths::isTestModeEnabled())
        return std::make_shared<Unavailable>();
#if defined(Q_OS_MACOS) || defined(Q_OS_LINUX) || defined(Q_OS_WIN)
    return platformCredentialBackend();
#else
    return std::make_shared<Unavailable>();
#endif
}
} // namespace ketplus::daemon

#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>

namespace ketplus::daemon {
class CredentialBackend;
class CredentialStore final {
  public:
    explicit CredentialStore(QString service = QStringLiteral("dev.ketplus.daemon.control"));
    // Explicit backend/path injection is for isolated fixtures; production uses the OS store.
    CredentialStore(QString service, std::shared_ptr<CredentialBackend> backend,
                    QString legacyPath);
    [[nodiscard]] QByteArray read(const QString& account, QString* error = nullptr) const;
    bool write(const QString& account, const QByteArray& value, QString* error = nullptr) const;
    bool remove(const QString& account, QString* error = nullptr) const;
    // Each batch replaces one OS record; partial account updates are never written.
    bool removeIf(const std::function<bool(const QString& account)>& match,
                  QString* error = nullptr) const;
    bool update(const std::function<void(QHash<QString, QByteArray>& accounts)>& change,
                QString* error = nullptr) const;

  private:
    bool transaction(const std::function<void(QHash<QString, QByteArray>&)>& change,
                     QHash<QString, QByteArray>* result, QString* error) const;
    QString service_;
    std::shared_ptr<CredentialBackend> backend_;
    QString legacyPath_;
};
} // namespace ketplus::daemon

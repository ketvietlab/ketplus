#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <functional>
#include <memory>
#include <mutex>

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
    // One synchronous operation, one locked OS read and at most one commit. The supplied
    // view must not escape the callback. Do not dispatch events, network calls or callbacks
    // while holding it. Returning false discards every staged write.
    bool batch(const std::function<bool(CredentialStore& view, QString* error)>& operation,
               QString* error = nullptr) const;

  private:
    bool transaction(const std::function<bool(QHash<QString, QByteArray>&)>& change,
                     QHash<QString, QByteArray>* result, QString* error) const;
    QString service_;
    std::shared_ptr<CredentialBackend> backend_;
    QString legacyPath_;
    QHash<QString, QByteArray>* stagedAccounts_{nullptr};
    // The last record this store read or committed. Every transaction still takes the lock,
    // and the lock file carries a generation that each commit advances, so a write by another
    // store or process is seen. Decrypting the OS record on every read was the cost.
    struct Cache {
        bool valid{false};
        QByteArray generation;
        QJsonObject record;
        QHash<QString, QByteArray> accounts;
    };
    mutable std::mutex cacheMutex_;
    mutable Cache cache_;
};
} // namespace ketplus::daemon

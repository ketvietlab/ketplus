#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QString>

#include <functional>

namespace ketplus::daemon {

class CredentialStore final {
  public:
    explicit CredentialStore(QString service = QStringLiteral("dev.ketplus.daemon.control"));

    [[nodiscard]] QByteArray read(const QString& account, QString* error = nullptr) const;
    bool write(const QString& account, const QByteArray& value, QString* error = nullptr) const;
    bool remove(const QString& account, QString* error = nullptr) const;
    // Removes every account `match` accepts with one rewrite of the file, or none when nothing
    // matches. Removing entries one by one rewrote the whole file for each of them.
    bool removeIf(const std::function<bool(const QString& account)>& match,
                  QString* error = nullptr) const;
    // Applies `change` to every stored account and its value, then stores the result with one
    // rewrite of the file: either every change is stored or none is. Nothing is written when
    // `change` leaves the accounts as they were.
    bool update(const std::function<void(QHash<QString, QByteArray>& accounts)>& change,
                QString* error = nullptr) const;

    // Identifies one version of the credential file on disk: device, inode, size, and
    // modification and status-change times in nanoseconds. A save renames a new file into place.
    struct FileStamp {
        qint64 device{-1};
        qint64 inode{-1};
        qint64 size{-1};
        qint64 modifiedNs{-1};
        qint64 changedNs{-1};
        bool operator==(const FileStamp&) const = default;
    };

  private:
    friend class CredentialStoreTestAccess;

    bool load(QJsonObject* values, QString* error) const;
    void remember(const QJsonObject& values) const;

    QString service_;
    // The parsed file, reused while the file on disk is the one it was read from. Every cloud
    // request reads keys from here, and parsing the file each time held the daemon's main thread.
    struct Snapshot {
        bool valid{false};
        FileStamp stamp;
        QJsonObject values;
    };
    mutable Snapshot snapshot_;
    mutable int fileReads_{0};
    mutable int fileWrites_{0};
};

} // namespace ketplus::daemon

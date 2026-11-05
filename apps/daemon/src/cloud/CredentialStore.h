#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace ketplus::daemon {

class CredentialStore final {
  public:
    explicit CredentialStore(QString service = QStringLiteral("dev.ketplus.daemon.control"));

    [[nodiscard]] QByteArray read(const QString& account, QString* error = nullptr) const;
    bool write(const QString& account, const QByteArray& value, QString* error = nullptr) const;
    bool remove(const QString& account, QString* error = nullptr) const;

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
};

} // namespace ketplus::daemon

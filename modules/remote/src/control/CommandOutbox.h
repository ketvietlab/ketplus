#pragma once
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

// The durable Desktop command outbox (docs/linux-headless-daemon.md, "Qt backend prepared for
// Desktop"). A prepared, encrypted command envelope is written here before it is sent; after a
// restart or a reconnect only that ciphertext is submitted again, so the relay's idempotency and
// the host's command journal see one command. Plaintext never reaches the file.
namespace ketplus::remote {

class CommandOutbox final {
  public:
    // How the envelope is submitted: to its session, or to its catalog worktree.
    enum class Route { Session, Worktree };

    struct Entry {
        Route route{Route::Session};
        QJsonObject envelope;
        // Milliseconds since the epoch; a command older than the relay's lifetime is given up.
        qint64 createdAt{0};
        [[nodiscard]] QString id() const { return envelope.value("id").toString(); }
    };

    explicit CommandOutbox(QString file);

    [[nodiscard]] const QString& file() const { return file_; }
    // Reads the file; a missing file is an empty outbox.
    bool load(QString* error = nullptr);
    [[nodiscard]] QList<Entry> pending() const { return pending_; }
    [[nodiscard]] bool isPending(const QString& id) const;
    // A command whose verified result was already applied; its result is never applied again.
    [[nodiscard]] bool isCompleted(const QString& id) const;
    // Persists the envelope before it is sent. Refuses an id that is pending or completed.
    bool add(Route route, const QJsonObject& envelope, QString* error = nullptr);
    // Removes an entry that was never sent, without remembering it as completed.
    bool withdraw(const QString& id, QString* error = nullptr);
    // Removes the entry and remembers its id (the most recent 1024 ids are kept).
    bool complete(const QString& id, QString* error = nullptr);

  private:
    bool save(QString* error);

    QString file_;
    QList<Entry> pending_;
    QStringList completed_;
};

} // namespace ketplus::remote

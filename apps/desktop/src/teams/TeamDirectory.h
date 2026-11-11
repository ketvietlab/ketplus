#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace ketplus {

// ADR 0114: the desktop's copy of the daemon's teams. The daemon owns every team; this cache
// reads them once (team.list), then follows team.changed and team.removed so a team created on
// another client appears without a reload. Every change goes back through the daemon.
class TeamDirectory final : public QObject {
    Q_OBJECT

  public:
    // `error` is empty on success; `result` is the daemon's payload either way.
    using Reply = std::function<void(const QJsonObject& result, const QString& error)>;
    using Transport =
        std::function<void(const QString& type, const QJsonObject& payload, Reply reply)>;

    explicit TeamDirectory(QObject* parent = nullptr);

    void setTransport(Transport transport);
    // Only a daemon that advertises teams/1 has teams. Turning it off forgets them.
    void setAvailable(bool available);
    [[nodiscard]] bool isAvailable() const noexcept { return available_; }

    void refresh();
    static constexpr int firstRetryMs = 1000;
    static constexpr int longestRetryMs = 30000;
    void setFirstRetryForTesting(int milliseconds) { retryDelayMs_ = firstRetry_ = milliseconds; }
    void applyEvent(const QString& type, const QJsonObject& payload);
    // Sends a team request; a "team" in the answer updates the cache at once.
    void request(const QString& type, const QJsonObject& payload, Reply reply = {});

    [[nodiscard]] QList<QJsonObject> teams() const;
    [[nodiscard]] QList<QJsonObject> teams(const QString& repository) const;
    [[nodiscard]] std::optional<QJsonObject> team(const QString& teamId) const;
    // The team a conversation (its daemon session id) belongs to.
    [[nodiscard]] std::optional<QJsonObject> teamOf(const QString& sessionId) const;

    [[nodiscard]] static QString normalizedRepository(const QString& path);
    [[nodiscard]] static QJsonObject member(const QJsonObject& team, const QString& sessionId);
    [[nodiscard]] static int count(const QJsonObject& team, const QString& role);

  signals:
    void changed();

  private:
    void store(const QJsonObject& team);

    Transport transport_;
    bool available_{false};
    // A list that failed is asked for again, later each time: the daemon may still be
    // starting, and without the list the sidebar would show a team's members as loose
    // conversations until something else changed.
    int retryDelayMs_{firstRetryMs};
    int listRequest_{0};
    int firstRetry_{firstRetryMs};
    QHash<QString, QJsonObject> teams_;
};

} // namespace ketplus

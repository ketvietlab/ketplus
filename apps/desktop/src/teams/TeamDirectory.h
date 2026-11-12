#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace ketplus {

class TeamCommands;

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
    // ADR 0115: the daemon went away. The teams last read stay, marked stale, so the sidebar
    // keeps each team's members together until the daemon is back and the list is read again.
    // Requests fail meanwhile.
    void setDisconnected();
    [[nodiscard]] bool isStale() const noexcept { return stale_; }
    // Whether the sidebar groups conversations by team: teams that are current or stale.
    [[nodiscard]] bool hasTeams() const noexcept { return available_ || stale_; }

    void refresh();
    static constexpr int firstRetryMs = 1000;
    static constexpr int longestRetryMs = 30000;
    void setFirstRetryForTesting(int milliseconds) { retryDelayMs_ = firstRetry_ = milliseconds; }
    void applyEvent(const QString& type, const QJsonObject& payload);
    // Sends a team request; a "team" in the answer updates the cache at once.
    void request(const QString& type, const QJsonObject& payload, Reply reply = {});
    // The commands the user's controls send: each shows at once that it was taken and is
    // sent once (TeamCommands.h). Shared by the team page, its dialogs and the window.
    [[nodiscard]] TeamCommands* commands();

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
    void store(const QJsonObject& received);

    Transport transport_;
    TeamCommands* commands_{nullptr};
    bool available_{false};
    bool stale_{false};
    // A list that failed is asked for again, later each time: the daemon may still be
    // starting, and without the list the sidebar would show a team's members as loose
    // conversations until something else changed.
    int retryDelayMs_{firstRetryMs};
    int listRequest_{0};
    int firstRetry_{firstRetryMs};
    QHash<QString, QJsonObject> teams_;
};

} // namespace ketplus

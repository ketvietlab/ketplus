#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <optional>

namespace ketplus {

// The desktop's only schedule client. The window, and later the new-conversation and
// new-team flows, talk to schedule.* through this cache. schedule.changed and
// schedule.run update it without another list.
class ScheduleClient final : public QObject {
    Q_OBJECT

  public:
    using Reply = std::function<void(const QJsonObject& result, const QString& error)>;
    using Transport =
        std::function<void(const QString& type, const QJsonObject& payload, Reply reply)>;

    // Newest-first pages small enough that Show more is real. The daemon keeps at most 50.
    static constexpr int runsPageSize = 20;

    explicit ScheduleClient(QObject* parent = nullptr);

    void setTransport(Transport transport);
    void setAvailable(bool available);
    [[nodiscard]] bool isAvailable() const noexcept { return available_; }

    void refresh();
    void applyEvent(const QString& type, const QJsonObject& payload);
    void request(const QString& type, const QJsonObject& payload, Reply reply = {});
    // `more` asks for the next older page. The first call replaces the page.
    void loadRuns(const QString& id, bool more);

    [[nodiscard]] QList<QJsonObject> schedules() const;
    [[nodiscard]] std::optional<QJsonObject> schedule(const QString& id) const;
    [[nodiscard]] QString listError() const { return listError_; }

    [[nodiscard]] QList<QJsonObject> runs(const QString& id) const;
    [[nodiscard]] bool runsLoaded(const QString& id) const;
    [[nodiscard]] bool runsHaveMore(const QString& id) const;
    [[nodiscard]] QJsonObject latestRun(const QString& id) const;

  signals:
    void changed();
    void runsChanged(const QString& id);

  private:
    struct RunPage {
        QList<QJsonObject> items;
        bool loaded{false};
        bool hasMore{false};
    };

    void store(const QJsonObject& schedule);
    void absorb(const QString& type, const QJsonObject& payload, const QJsonObject& result);
    void prependRun(const QJsonObject& run);

    Transport transport_;
    bool available_{false};
    int listRequest_{0};
    QString listError_;
    QHash<QString, QJsonObject> schedules_;
    QHash<QString, RunPage> runs_;
    QHash<QString, QJsonObject> latestRun_;
};

} // namespace ketplus

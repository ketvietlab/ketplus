#pragma once

#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTimeZone>

namespace ketplus {

class ScheduleClient;

// Composer clock for a new conversation's first message, and for an existing
// conversation that already has a schedule. The desktop has one ScheduleClient;
// this object only decides the icon, the popover draft, the create request,
// the scheduled-run marker and the failure toast.
class ComposerSchedule final : public QObject {
    Q_OBJECT
    Q_PROPERTY(int mode READ mode WRITE setMode NOTIFY changed)
    Q_PROPERTY(bool newConversation READ newConversation NOTIFY changed)
    Q_PROPERTY(bool scheduleSet READ scheduleSet NOTIFY changed)
    Q_PROPERTY(bool clockVisible READ clockVisible NOTIFY changed)
    Q_PROPERTY(bool clockActive READ clockActive NOTIFY changed)
    Q_PROPERTY(QString tooltip READ tooltip NOTIFY changed)
    Q_PROPERTY(QString summary READ summary NOTIFY changed)
    Q_PROPERTY(QString draftPrompt READ draftPrompt WRITE setDraftPrompt NOTIFY changed)
    Q_PROPERTY(bool draftKept READ draftKept NOTIFY changed)
    Q_PROPERTY(QString lastError READ lastError NOTIFY changed)
    Q_PROPERTY(QString time READ time WRITE setTime NOTIFY changed)
    Q_PROPERTY(QString date READ date WRITE setDate NOTIFY changed)
    Q_PROPERTY(QString zone READ zone WRITE setZone NOTIFY changed)
    Q_PROPERTY(QString repeatKind READ repeatKind WRITE setRepeatKind NOTIFY changed)
    Q_PROPERTY(QStringList weekDays READ weekDays WRITE setWeekDays NOTIFY changed)
    Q_PROPERTY(int everyHours READ everyHours WRITE setEveryHours NOTIFY changed)
    Q_PROPERTY(bool newEachRun READ newEachRun WRITE setNewEachRun NOTIFY changed)
    Q_PROPERTY(QString busyPolicy READ busyPolicy WRITE setBusyPolicy NOTIFY changed)
    Q_PROPERTY(QString missedPolicy READ missedPolicy WRITE setMissedPolicy NOTIFY changed)
    Q_PROPERTY(int repeatIndex READ repeatIndex WRITE setRepeatIndex NOTIFY changed)
    Q_PROPERTY(int busyIndex READ busyIndex WRITE setBusyIndex NOTIFY changed)
    Q_PROPERTY(int missedIndex READ missedIndex WRITE setMissedIndex NOTIFY changed)
    Q_PROPERTY(int zoneIndex READ zoneIndex WRITE setZoneIndex NOTIFY changed)
    Q_PROPERTY(QStringList zones READ zones NOTIFY changed)
    Q_PROPERTY(QString liveSummary READ liveSummary NOTIFY changed)
    Q_PROPERTY(QString atTimeHint READ atTimeHint NOTIFY changed)
    Q_PROPERTY(QString existingName READ existingName NOTIFY changed)
    Q_PROPERTY(QString actionLabel READ actionLabel CONSTANT)

  public:
    explicit ComposerSchedule(QObject* parent = nullptr);

    void setClient(ScheduleClient* client);

    [[nodiscard]] int mode() const noexcept { return mode_; }
    void setMode(int mode);
    [[nodiscard]] bool newConversation() const noexcept { return newConversation_; }
    [[nodiscard]] bool scheduleSet() const noexcept { return scheduleSet_; }
    [[nodiscard]] bool clockVisible() const;
    [[nodiscard]] bool clockActive() const;
    [[nodiscard]] QString tooltip() const;
    [[nodiscard]] QString summary() const;
    [[nodiscard]] QString draftPrompt() const { return draftPrompt_; }
    void setDraftPrompt(const QString& prompt);
    [[nodiscard]] bool draftKept() const noexcept { return draftKept_; }
    [[nodiscard]] QString lastError() const { return lastError_; }
    [[nodiscard]] QString time() const { return time_; }
    void setTime(const QString& time);
    [[nodiscard]] QString date() const { return date_; }
    void setDate(const QString& date);
    [[nodiscard]] QString zone() const { return zone_; }
    void setZone(const QString& zone);
    [[nodiscard]] QString repeatKind() const { return repeatKind_; }
    void setRepeatKind(const QString& kind);
    [[nodiscard]] QStringList weekDays() const { return weekDays_; }
    void setWeekDays(const QStringList& days);
    [[nodiscard]] int everyHours() const noexcept { return everyHours_; }
    void setEveryHours(int hours);
    [[nodiscard]] bool newEachRun() const noexcept { return newEachRun_; }
    void setNewEachRun(bool each);
    [[nodiscard]] QString busyPolicy() const { return busyPolicy_; }
    void setBusyPolicy(const QString& policy);
    [[nodiscard]] QString missedPolicy() const { return missedPolicy_; }
    void setMissedPolicy(const QString& policy);
    [[nodiscard]] int repeatIndex() const;
    void setRepeatIndex(int index);
    [[nodiscard]] int busyIndex() const;
    void setBusyIndex(int index);
    [[nodiscard]] int missedIndex() const;
    void setMissedIndex(int index);
    [[nodiscard]] int zoneIndex() const;
    void setZoneIndex(int index);
    [[nodiscard]] QStringList zones() const;
    [[nodiscard]] QString liveSummary() const;
    [[nodiscard]] QString atTimeHint() const;
    [[nodiscard]] QString existingName() const;
    [[nodiscard]] static QString actionLabel() { return QStringLiteral("Open schedule"); }
    Q_INVOKABLE void applyPreset(const QString& preset);

    // A conversation with no messages yet can create a schedule. Any other
    // conversation only shows the clock when one of its schedules matches.
    Q_INVOKABLE void setContext(bool fresh, const QString& sessionId, const QString& workspace,
                                const QString& provider, const QString& model,
                                const QString& effort);
    // Send now clears a draft. At a time and Repeat keep one when the fields validate.
    Q_INVOKABLE bool apply();
    // True when Send created (or tried to create) a schedule instead of sending.
    // A daemon error leaves draftPrompt unchanged.
    Q_INVOKABLE bool send();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void edit();
    Q_INVOKABLE void openSchedules();
    Q_INVOKABLE void openSchedule(const QString& id);
    Q_INVOKABLE QString markerText(const QString& payloadJson) const;
    Q_INVOKABLE QString markerScheduleId(const QString& payloadJson) const;
    // Empty for a successful run. Failed, skipped and missed runs return the reason.
    Q_INVOKABLE QString toastText(const QString& status, const QString& reason) const;
    Q_INVOKABLE void observeRun(const QJsonObject& run);

  signals:
    void changed();
    void created(const QString& id);
    void sendFailed(const QString& error);
    void toastRequested(const QString& text, const QString& scheduleId);
    void openRequested(const QString& scheduleId);
    void editRequested(const QString& scheduleId);

  private:
    [[nodiscard]] QJsonObject existing() const;
    [[nodiscard]] QString ruleText() const;
    [[nodiscard]] QString validate() const;
    [[nodiscard]] QString localClock(const QString& iso) const;
    [[nodiscard]] QString failureSentence(const QString& name, const QString& status,
                                          const QString& reason) const;
    [[nodiscard]] QString offsetLabel(const QTimeZone& zone) const;
    [[nodiscard]] QString nextLabel() const;
    [[nodiscard]] QJsonObject createPayload() const;
    void rememberRuns(bool announce);
    void publish();

    ScheduleClient* client_{nullptr};
    bool seeded_{false};
    QSet<QString> seenRuns_;
    bool newConversation_{true};
    QString sessionId_;
    QString workspace_;
    QString provider_;
    QString model_;
    QString effort_;
    int mode_{0};
    bool scheduleSet_{false};
    QString draftPrompt_;
    bool draftKept_{false};
    QString lastError_;
    QString time_{QStringLiteral("09:00")};
    QString date_;
    QString zone_{QStringLiteral("UTC")};
    QString repeatKind_{QStringLiteral("daily")};
    QStringList weekDays_{QStringLiteral("mon"), QStringLiteral("tue"), QStringLiteral("wed"),
                          QStringLiteral("thu"), QStringLiteral("fri")};
    int everyHours_{4};
    bool newEachRun_{false};
    QString busyPolicy_{QStringLiteral("queue")};
    QString missedPolicy_{QStringLiteral("run_once")};
    QString savedRule_;
    QString savedSummary_;
};

} // namespace ketplus

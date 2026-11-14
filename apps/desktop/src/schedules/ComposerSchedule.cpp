#include "schedules/ComposerSchedule.h"

#include "schedules/ScheduleClient.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTime>
#include <QTimeZone>

namespace ketplus {
namespace {

QString compact(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QJsonObject objectFrom(const QString& json) {
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    return document.isObject() ? document.object() : QJsonObject{};
}

QString clock(const QString& iso) {
    if (iso.size() >= 16 && iso.at(10) == QLatin1Char('T'))
        return iso.left(10) + QStringLiteral(" ") + iso.mid(11, 5);
    return iso;
}

bool recent(const QString& iso) {
    if (iso.isEmpty())
        return true;
    const QDateTime when = QDateTime::fromString(iso, Qt::ISODateWithMs);
    const QDateTime plain = when.isValid() ? when : QDateTime::fromString(iso, Qt::ISODate);
    if (!plain.isValid())
        return false;
    return plain.toUTC().secsTo(QDateTime::currentDateTimeUtc()) <= 90 &&
           plain.toUTC().secsTo(QDateTime::currentDateTimeUtc()) >= -5;
}

} // namespace

ComposerSchedule::ComposerSchedule(QObject* parent) : QObject(parent) {
    const QTimeZone zone = QTimeZone::systemTimeZone();
    const QByteArray id = zone.id();
    if (!id.isEmpty() && !id.startsWith("GMT") && !id.startsWith("UTC"))
        zone_ = QString::fromUtf8(id);
    date_ = QDateTime::currentDateTime().date().toString(QStringLiteral("yyyy-MM-dd"));
}

void ComposerSchedule::setClient(ScheduleClient* client) {
    if (client_ == client)
        return;
    if (client_ != nullptr)
        disconnect(client_, nullptr, this, nullptr);
    client_ = client;
    seenRuns_.clear();
    seeded_ = client_ == nullptr;
    if (client_ == nullptr) {
        publish();
        return;
    }
    connect(client_, &ScheduleClient::changed, this, [this] { rememberRuns(true); });
    rememberRuns(false);
    seeded_ = true;
    publish();
}

void ComposerSchedule::setMode(const int mode) {
    const int next = mode < 0 ? 0 : mode > 2 ? 2 : mode;
    if (next == mode_)
        return;
    mode_ = next;
    publish();
}

void ComposerSchedule::setDraftPrompt(const QString& prompt) {
    if (prompt == draftPrompt_)
        return;
    draftPrompt_ = prompt;
    publish();
}

void ComposerSchedule::setTime(const QString& time) {
    if (time == time_)
        return;
    time_ = time;
    publish();
}

void ComposerSchedule::setDate(const QString& date) {
    if (date == date_)
        return;
    date_ = date;
    publish();
}

void ComposerSchedule::setZone(const QString& zone) {
    if (zone == zone_)
        return;
    zone_ = zone;
    publish();
}

void ComposerSchedule::setRepeatKind(const QString& kind) {
    if (kind == repeatKind_)
        return;
    repeatKind_ = kind;
    publish();
}

void ComposerSchedule::setWeekDays(const QStringList& days) {
    if (days == weekDays_)
        return;
    weekDays_ = days;
    publish();
}

void ComposerSchedule::setEveryHours(const int hours) {
    if (hours == everyHours_)
        return;
    everyHours_ = hours;
    publish();
}

void ComposerSchedule::setNewEachRun(const bool each) {
    if (each == newEachRun_)
        return;
    newEachRun_ = each;
    publish();
}

void ComposerSchedule::setBusyPolicy(const QString& policy) {
    if (policy == busyPolicy_)
        return;
    busyPolicy_ = policy;
    publish();
}

void ComposerSchedule::setMissedPolicy(const QString& policy) {
    if (policy == missedPolicy_)
        return;
    missedPolicy_ = policy;
    publish();
}

void ComposerSchedule::setContext(const bool fresh, const QString& sessionId,
                                  const QString& workspace, const QString& provider,
                                  const QString& model, const QString& effort) {
    if (fresh == newConversation_ && sessionId == sessionId_ && workspace == workspace_ &&
        provider == provider_ && model == model_ && effort == effort_)
        return;
    newConversation_ = fresh;
    sessionId_ = sessionId;
    workspace_ = workspace;
    provider_ = provider;
    model_ = model;
    effort_ = effort;
    publish();
}

QJsonObject ComposerSchedule::existing() const {
    if (client_ == nullptr || sessionId_.isEmpty())
        return {};
    for (const schedule::Schedule& schedule : client_->schedules()) {
        if (schedule.target.kind == schedule::TargetKind::Conversation &&
            schedule.target.sessionId == sessionId_)
            return schedule::toJson(schedule);
    }
    return {};
}

bool ComposerSchedule::clockVisible() const { return newConversation_ || !existing().isEmpty(); }

bool ComposerSchedule::clockActive() const {
    if (newConversation_)
        return scheduleSet_;
    const QJsonObject schedule = existing();
    return !schedule.isEmpty() && schedule.value(QStringLiteral("enabled")).toBool();
}

QString ComposerSchedule::summary() const {
    if (!newConversation_) {
        const QJsonObject schedule = existing();
        if (schedule.isEmpty())
            return {};
        if (!schedule.value(QStringLiteral("enabled")).toBool())
            return QStringLiteral("Paused");
        const QString next = schedule.value(QStringLiteral("next_run_at")).toString();
        return next.isEmpty() ? QStringLiteral("Scheduled") : clock(next);
    }
    if (!scheduleSet_)
        return {};
    return savedSummary_;
}

QString ComposerSchedule::tooltip() const {
    if (!newConversation_) {
        const QJsonObject schedule = existing();
        if (schedule.isEmpty())
            return {};
        if (!schedule.value(QStringLiteral("enabled")).toBool())
            return QStringLiteral("Schedule paused");
        const QString next = summary();
        return next.isEmpty() ? QStringLiteral("Next run")
                              : QStringLiteral("Next run: %1").arg(next);
    }
    if (!scheduleSet_)
        return QStringLiteral("Schedule first message");
    return QStringLiteral("Scheduled · %1").arg(savedSummary_);
}

QString ComposerSchedule::ruleText() const {
    QJsonObject rule{{QStringLiteral("time_zone"), zone_}};
    if (mode_ == 1) {
        rule.insert(QStringLiteral("kind"), QStringLiteral("once"));
        rule.insert(QStringLiteral("at"), date_ + QStringLiteral("T") + time_);
        return compact(rule);
    }
    if (repeatKind_ == QStringLiteral("interval")) {
        const QString anchor = date_ + QStringLiteral("T") + time_;
        rule.insert(QStringLiteral("kind"), QStringLiteral("interval"));
        rule.insert(QStringLiteral("every_hours"), everyHours_);
        rule.insert(QStringLiteral("anchor"), anchor);
        return compact(rule);
    }
    rule.insert(QStringLiteral("kind"), repeatKind_);
    rule.insert(QStringLiteral("time"), time_);
    if (repeatKind_ == QStringLiteral("weekly"))
        rule.insert(QStringLiteral("days"), QJsonArray::fromStringList(weekDays_));
    return compact(rule);
}

QString ComposerSchedule::validate() const {
    if (zone_.trimmed().isEmpty())
        return QStringLiteral("Choose a timezone.");
    if (!QTime::fromString(time_, QStringLiteral("HH:mm")).isValid())
        return QStringLiteral("Enter a time as HH:mm.");
    if (mode_ == 1 || repeatKind_ == QStringLiteral("interval")) {
        if (!QDate::fromString(date_, QStringLiteral("yyyy-MM-dd")).isValid())
            return QStringLiteral("Enter a date as YYYY-MM-DD.");
    }
    if (mode_ == 2 && repeatKind_ == QStringLiteral("weekly") && weekDays_.isEmpty())
        return QStringLiteral("Choose at least one day.");
    if (mode_ == 2 && repeatKind_ == QStringLiteral("interval") &&
        (everyHours_ < 1 || everyHours_ > 24))
        return QStringLiteral("Repeat every 1 to 24 hours.");
    if (mode_ == 2 && repeatKind_ != QStringLiteral("daily") &&
        repeatKind_ != QStringLiteral("weekdays") && repeatKind_ != QStringLiteral("weekly") &&
        repeatKind_ != QStringLiteral("interval"))
        return QStringLiteral("Choose how the schedule repeats.");
    return {};
}

bool ComposerSchedule::apply() {
    lastError_.clear();
    if (mode_ == 0) {
        scheduleSet_ = false;
        savedRule_.clear();
        savedSummary_.clear();
        publish();
        return true;
    }
    const QString problem = validate();
    if (!problem.isEmpty()) {
        lastError_ = problem;
        publish();
        return false;
    }
    savedRule_ = ruleText();
    if (mode_ == 1)
        savedSummary_ = QStringLiteral("Once · %1 %2").arg(date_, time_);
    else if (repeatKind_ == QStringLiteral("daily"))
        savedSummary_ = QStringLiteral("Daily %1").arg(time_);
    else if (repeatKind_ == QStringLiteral("weekdays"))
        savedSummary_ = QStringLiteral("Weekdays %1").arg(time_);
    else if (repeatKind_ == QStringLiteral("weekly"))
        savedSummary_ = QStringLiteral("Weekly %1").arg(time_);
    else if (everyHours_ == 1)
        savedSummary_ = QStringLiteral("Every hour");
    else
        savedSummary_ = QStringLiteral("Every %1 hours").arg(everyHours_);
    scheduleSet_ = true;
    publish();
    return true;
}

QJsonObject ComposerSchedule::createPayload() const {
    const QString prompt = draftPrompt_.trimmed();
    QString name = prompt.section(QLatin1Char('\n'), 0, 0).trimmed();
    if (name.size() > 60)
        name.resize(60);
    if (name.isEmpty())
        name = QStringLiteral("First message");
    return {{QStringLiteral("name"), name},
            {QStringLiteral("enabled"), true},
            {QStringLiteral("prompt"), prompt},
            {QStringLiteral("target"), QJsonObject{{QStringLiteral("kind"), QStringLiteral("new")},
                                                   {QStringLiteral("workspace"), workspace_},
                                                   {QStringLiteral("provider"), provider_},
                                                   {QStringLiteral("model"), model_},
                                                   {QStringLiteral("effort"), effort_},
                                                   {QStringLiteral("isolated_worktree"), false},
                                                   {QStringLiteral("title_template"), QString{}}}},
            {QStringLiteral("rule"), savedRule_},
            {QStringLiteral("busy_policy"), busyPolicy_},
            {QStringLiteral("missed_policy"), missedPolicy_}};
}

bool ComposerSchedule::send() {
    if (!newConversation_ || !scheduleSet_)
        return false;
    const QString prompt = draftPrompt_;
    if (prompt.trimmed().isEmpty()) {
        lastError_ = QStringLiteral("Write the first message before setting a schedule.");
        draftKept_ = true;
        publish();
        emit sendFailed(lastError_);
        return true;
    }
    if (workspace_.trimmed().isEmpty()) {
        lastError_ = QStringLiteral("Choose a workspace before scheduling.");
        draftKept_ = true;
        publish();
        emit sendFailed(lastError_);
        return true;
    }
    if (client_ == nullptr) {
        lastError_ = QStringLiteral("Schedules need a KetPlus Daemon that supports them.");
        draftKept_ = true;
        publish();
        emit sendFailed(lastError_);
        return true;
    }
    const QJsonObject payload = createPayload();
    const QPointer<ComposerSchedule> alive(this);
    client_->request(QStringLiteral("schedule.create"), payload,
                     [alive, prompt](const QJsonObject& result, const QString& error) {
                         if (!alive)
                             return;
                         if (!error.isEmpty()) {
                             alive->draftPrompt_ = prompt;
                             alive->draftKept_ = true;
                             alive->lastError_ = error;
                             alive->publish();
                             emit alive->sendFailed(error);
                             return;
                         }
                         alive->draftKept_ = false;
                         alive->scheduleSet_ = false;
                         alive->mode_ = 0;
                         alive->lastError_.clear();
                         const QString id = result.value(QStringLiteral("schedule"))
                                                .toObject()
                                                .value(QStringLiteral("id"))
                                                .toString();
                         alive->publish();
                         emit alive->created(id);
                     });
    return true;
}

void ComposerSchedule::pause() {
    const QJsonObject schedule = existing();
    if (schedule.isEmpty() || client_ == nullptr)
        return;
    client_->request(
        QStringLiteral("schedule.setEnabled"),
        {{QStringLiteral("id"), schedule.value(QStringLiteral("id")).toString()},
         {QStringLiteral("enabled"), !schedule.value(QStringLiteral("enabled")).toBool()}});
}

void ComposerSchedule::edit() { openSchedules(); }

void ComposerSchedule::openSchedules() {
    const QJsonObject schedule = existing();
    emit openRequested(schedule.value(QStringLiteral("id")).toString());
}

void ComposerSchedule::openSchedule(const QString& id) { emit openRequested(id); }

QString ComposerSchedule::markerScheduleId(const QString& payloadJson) const {
    return objectFrom(payloadJson).value(QStringLiteral("schedule_id")).toString();
}

QString ComposerSchedule::markerText(const QString& payloadJson) const {
    const QJsonObject payload = objectFrom(payloadJson);
    const QString id = payload.value(QStringLiteral("schedule_id")).toString();
    if (id.isEmpty())
        return {};
    QString name = QStringLiteral("Schedule");
    QString when = clock(payload.value(QStringLiteral("scheduled_for")).toString());
    if (client_ != nullptr) {
        if (const auto schedule = client_->schedule(id)) {
            if (!schedule->name.isEmpty())
                name = schedule->name;
            if (when.isEmpty())
                when = clock(schedule->nextRunAt);
        }
        const schedule::Run run = client_->latestRun(id);
        if (when.isEmpty())
            when = clock(run.scheduledFor);
    }
    if (when.isEmpty())
        return QStringLiteral("Scheduled run: %1").arg(name);
    return QStringLiteral("Scheduled run: %1 · %2").arg(name, when);
}

QString ComposerSchedule::toastText(const QString& status, const QString& reason) const {
    if (status != QStringLiteral("failed") && status != QStringLiteral("skipped") &&
        status != QStringLiteral("missed"))
        return {};
    const QString detail = reason.trimmed().isEmpty() ? status : reason.trimmed();
    return detail;
}

void ComposerSchedule::observeRun(const QJsonObject& run) {
    const QString id = run.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || seenRuns_.contains(id))
        return;
    seenRuns_.insert(id);
    const QString status = run.value(QStringLiteral("status")).toString();
    const QString text = toastText(status, run.value(QStringLiteral("error_message")).toString());
    if (text.isEmpty() || !recent(run.value(QStringLiteral("finished_at")).toString()))
        return;
    emit toastRequested(text, run.value(QStringLiteral("schedule_id")).toString());
}

void ComposerSchedule::rememberRuns(const bool announce) {
    if (client_ == nullptr)
        return;
    for (const schedule::Schedule& schedule : client_->schedules()) {
        const schedule::Run run = client_->latestRun(schedule.id);
        if (run.id.isEmpty())
            continue;
        if (!announce) {
            seenRuns_.insert(run.id);
            continue;
        }
        observeRun(schedule::toJson(run));
    }
    publish();
}

void ComposerSchedule::publish() { emit changed(); }

} // namespace ketplus

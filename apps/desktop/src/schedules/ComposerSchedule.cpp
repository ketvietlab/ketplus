#include "schedules/ComposerSchedule.h"

#include "schedules/ScheduleClient.h"

#include <QDate>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QTime>
#include <QTimeZone>

#include <cstdlib>

namespace ketplus {
namespace {

QString compact(const QJsonObject& object) {
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QJsonObject objectFrom(const QString& json) {
    const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8());
    return document.isObject() ? document.object() : QJsonObject{};
}

QStringList zoneChoices(const QString& current) {
    QStringList zones{QStringLiteral("Asia/Ho_Chi_Minh"), QStringLiteral("UTC"),
                      QStringLiteral("Asia/Bangkok"),     QStringLiteral("Asia/Singapore"),
                      QStringLiteral("Asia/Tokyo"),       QStringLiteral("Europe/London"),
                      QStringLiteral("America/New_York"), QStringLiteral("America/Los_Angeles"),
                      QStringLiteral("Australia/Sydney")};
    if (!current.isEmpty() && !zones.contains(current))
        zones.prepend(current);
    return zones;
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

int ComposerSchedule::repeatIndex() const {
    if (repeatKind_ == QLatin1String("weekdays"))
        return 1;
    if (repeatKind_ == QLatin1String("weekly"))
        return 2;
    if (repeatKind_ == QLatin1String("interval"))
        return 3;
    return 0;
}

void ComposerSchedule::setRepeatIndex(const int index) {
    static const QStringList kinds{QStringLiteral("daily"), QStringLiteral("weekdays"),
                                   QStringLiteral("weekly"), QStringLiteral("interval")};
    if (index < 0 || index >= kinds.size())
        return;
    setRepeatKind(kinds.at(index));
}

int ComposerSchedule::busyIndex() const { return busyPolicy_ == QLatin1String("skip") ? 1 : 0; }

void ComposerSchedule::setBusyIndex(const int index) {
    setBusyPolicy(index == 1 ? QStringLiteral("skip") : QStringLiteral("queue"));
}

int ComposerSchedule::missedIndex() const { return missedPolicy_ == QLatin1String("skip") ? 1 : 0; }

void ComposerSchedule::setMissedIndex(const int index) {
    setMissedPolicy(index == 1 ? QStringLiteral("skip") : QStringLiteral("run_once"));
}

QStringList ComposerSchedule::zones() const { return zoneChoices(zone_); }

int ComposerSchedule::zoneIndex() const {
    const int index = static_cast<int>(zones().indexOf(zone_));
    return index < 0 ? 0 : index;
}

void ComposerSchedule::setZoneIndex(const int index) {
    const QStringList choices = zones();
    if (index < 0 || index >= choices.size())
        return;
    setZone(choices.at(index));
}

QString ComposerSchedule::localClock(const QString& iso) const {
    QDateTime when = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!when.isValid())
        when = QDateTime::fromString(iso, Qt::ISODate);
    if (!when.isValid())
        return iso;
    return when.toLocalTime().toString(QStringLiteral("dd/MM HH:mm"));
}

QString ComposerSchedule::offsetLabel(const QTimeZone& zone) const {
    const int seconds = zone.offsetFromUtc(QDateTime::currentDateTimeUtc());
    const int hours = seconds / 3600;
    const int minutes = std::abs(seconds % 3600) / 60;
    if (hours == 0 && minutes == 0)
        return QStringLiteral("GMT");
    const QString sign = seconds >= 0 ? QStringLiteral("+") : QStringLiteral("-");
    if (minutes == 0)
        return QStringLiteral("GMT%1%2").arg(sign).arg(std::abs(hours));
    return QStringLiteral("GMT%1%2:%3")
        .arg(sign)
        .arg(std::abs(hours), 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'));
}

QString ComposerSchedule::nextLabel() const {
    const QTime time = QTime::fromString(time_, QStringLiteral("HH:mm"));
    if (!time.isValid())
        return QStringLiteral("choose a time");
    QTimeZone zone(zone_.toUtf8());
    if (!zone.isValid())
        zone = QTimeZone::UTC;
    const QDateTime now = QDateTime::currentDateTimeUtc().toTimeZone(zone);
    if (mode_ == 2 && repeatKind_ == QLatin1String("interval")) {
        const QDateTime at = now.addSecs(static_cast<qint64>(everyHours_) * 3600);
        const int delta = now.date().daysTo(at.date());
        const QString day = delta <= 0   ? QStringLiteral("today")
                            : delta == 1 ? QStringLiteral("tomorrow")
                                         : at.toString(QStringLiteral("dd/MM"));
        return day + QLatin1Char(' ') + at.toString(QStringLiteral("HH:mm"));
    }
    static const QStringList keys{
        QStringLiteral("mon"), QStringLiteral("tue"), QStringLiteral("wed"), QStringLiteral("thu"),
        QStringLiteral("fri"), QStringLiteral("sat"), QStringLiteral("sun")};
    for (int day = 0; day < 8; ++day) {
        const QDateTime at(now.date().addDays(day), time, zone);
        if (at <= now)
            continue;
        const int weekday = at.date().dayOfWeek();
        if (mode_ == 2 && repeatKind_ == QLatin1String("weekdays") && weekday >= 6)
            continue;
        if (mode_ == 2 && repeatKind_ == QLatin1String("weekly") &&
            !weekDays_.contains(keys.at(weekday - 1)))
            continue;
        const int delta = now.date().daysTo(at.date());
        const QString label = delta == 0   ? QStringLiteral("today")
                              : delta == 1 ? QStringLiteral("tomorrow")
                                           : at.toString(QStringLiteral("dd/MM"));
        return label + QLatin1Char(' ') + time_;
    }
    return QStringLiteral("choose a day");
}

QString ComposerSchedule::liveSummary() const {
    QString label;
    if (mode_ == 1)
        label = QStringLiteral("Once · %1 %2").arg(date_, time_);
    else if (repeatKind_ == QLatin1String("weekdays"))
        label = QStringLiteral("Weekdays %1").arg(time_);
    else if (repeatKind_ == QLatin1String("weekly"))
        label = QStringLiteral("Weekly %1").arg(time_);
    else if (repeatKind_ == QLatin1String("interval"))
        label = everyHours_ == 1 ? QStringLiteral("Every hour")
                                 : QStringLiteral("Every %1 hours").arg(everyHours_);
    else
        label = QStringLiteral("Daily %1").arg(time_);
    QTimeZone zone(zone_.toUtf8());
    if (!zone.isValid())
        zone = QTimeZone::UTC;
    return QStringLiteral("%1 (%2) · next: %3").arg(label, offsetLabel(zone), nextLabel());
}

QString ComposerSchedule::atTimeHint() const {
    return QStringLiteral("→ The first message will be sent at %1").arg(time_);
}

QString ComposerSchedule::existingName() const {
    const QString name = existing().value(QStringLiteral("name")).toString();
    return name.isEmpty() ? QStringLiteral("Schedule") : name;
}

void ComposerSchedule::applyPreset(const QString& preset) {
    QTimeZone zone(zone_.toUtf8());
    if (!zone.isValid())
        zone = QTimeZone::systemTimeZone();
    const QDate today = QDateTime::currentDateTimeUtc().toTimeZone(zone).date();
    if (preset == QLatin1String("tomorrow")) {
        setDate(today.addDays(1).toString(QStringLiteral("yyyy-MM-dd")));
        setTime(QStringLiteral("08:00"));
        return;
    }
    setDate(today.toString(QStringLiteral("yyyy-MM-dd")));
    setTime(QStringLiteral("22:00"));
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
        return next.isEmpty() ? QStringLiteral("Scheduled") : localClock(next);
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

void ComposerSchedule::edit() {
    emit editRequested(existing().value(QStringLiteral("id")).toString());
}

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
    QString when = localClock(payload.value(QStringLiteral("scheduled_for")).toString());
    const QString runId = payload.value(QStringLiteral("run_id")).toString();
    if (client_ != nullptr) {
        if (const auto schedule = client_->schedule(id)) {
            if (!schedule->name.isEmpty())
                name = schedule->name;
        }
        if (when.isEmpty() && !runId.isEmpty()) {
            if (const auto run = client_->run(runId))
                when = localClock(run->scheduledFor);
        }
    }
    if (when.isEmpty())
        return QStringLiteral("Scheduled run: %1").arg(name);
    return QStringLiteral("Scheduled run: %1 · %2").arg(name, when);
}

QString ComposerSchedule::failureSentence(const QString& name, const QString& status,
                                          const QString& reason) const {
    if (status != QStringLiteral("failed") && status != QStringLiteral("skipped") &&
        status != QStringLiteral("missed"))
        return {};
    const QString detail = reason.trimmed().isEmpty() ? status : reason.trimmed();
    const QString title = name.isEmpty() ? QStringLiteral("Schedule") : name;
    return QStringLiteral("Schedule “%1” %2 — %3").arg(title, status, detail);
}

QString ComposerSchedule::toastText(const QString& status, const QString& reason) const {
    return failureSentence(QStringLiteral("Schedule"), status, reason);
}

void ComposerSchedule::observeRun(const QJsonObject& run) {
    const QString id = run.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || seenRuns_.contains(id))
        return;
    seenRuns_.insert(id);
    const QString status = run.value(QStringLiteral("status")).toString();
    QString name = QStringLiteral("Schedule");
    if (client_ != nullptr) {
        if (const auto schedule =
                client_->schedule(run.value(QStringLiteral("schedule_id")).toString()))
            if (!schedule->name.isEmpty())
                name = schedule->name;
    }
    const QString text =
        failureSentence(name, status, run.value(QStringLiteral("error_message")).toString());
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

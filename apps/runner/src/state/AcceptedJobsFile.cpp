#include "state/AcceptedJobsFile.h"

#include "state/StateDirectory.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace ketplus::runner::app {

namespace {

const QString fileName = QStringLiteral("accepted.json");
constexpr int fileVersion = 1;

bool validJobId(const QString& jobId) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return pattern.match(jobId).hasMatch();
}

QString utc(const QDateTime& time) { return time.toUTC().toString(Qt::ISODate); }

} // namespace

std::optional<AcceptedJobsFile> AcceptedJobsFile::load(const StateDirectory& directory,
                                                       std::function<QDateTime()> now,
                                                       QString* error) {
    AcceptedJobsFile file(directory, std::move(now));
    const StateDirectory::ReadResult read = directory.read(fileName);
    if (read.status == StateDirectory::ReadResult::Status::Missing)
        return file;
    const QString corrupt =
        QStringLiteral("%1/%2 is corrupt; it was left as it is").arg(directory.path(), fileName);
    if (read.status == StateDirectory::ReadResult::Status::Invalid) {
        *error = read.error;
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(read.data);
    const QJsonObject root = document.object();
    if (!document.isObject() ||
        root.value(QStringLiteral("version")).toInteger(-1) != fileVersion ||
        !root.value(QStringLiteral("jobs")).isArray()) {
        *error = corrupt;
        return std::nullopt;
    }
    for (const QJsonValue& value : root.value(QStringLiteral("jobs")).toArray()) {
        const QJsonObject job = value.toObject();
        Record record{
            job.value(QStringLiteral("jobId")).toString(),
            job.value(QStringLiteral("attempt")).toInteger(0),
            QDateTime::fromString(job.value(QStringLiteral("acceptedAt")).toString(), Qt::ISODate)};
        if (!value.isObject() || !validJobId(record.jobId) || record.attempt < 1 ||
            !record.acceptedAt.isValid()) {
            *error = corrupt;
            return std::nullopt;
        }
        file.m_jobs.append(record);
    }
    file.dropExpired();
    return file;
}

bool AcceptedJobsFile::contains(const QString& jobId, const qint64 attempt) const {
    const QDateTime oldest = m_now().addDays(-acceptedJobsKeepDays);
    return std::any_of(m_jobs.cbegin(), m_jobs.cend(), [&](const Record& record) {
        return record.jobId == jobId && record.attempt == attempt && record.acceptedAt > oldest;
    });
}

bool AcceptedJobsFile::add(const QString& jobId, const qint64 attempt) {
    if (contains(jobId, attempt))
        return true;
    QList<Record> jobs = m_jobs;
    jobs.append({jobId, attempt, m_now().toUTC()});
    QJsonArray array;
    const QDateTime oldest = m_now().addDays(-acceptedJobsKeepDays);
    for (const Record& record : jobs)
        if (record.acceptedAt > oldest)
            array.append(QJsonObject{{QStringLiteral("jobId"), record.jobId},
                                     {QStringLiteral("attempt"), record.attempt},
                                     {QStringLiteral("acceptedAt"), utc(record.acceptedAt)}});
    const QJsonObject root{{QStringLiteral("version"), fileVersion},
                           {QStringLiteral("jobs"), array}};
    if (!m_directory->write(fileName, QJsonDocument(root).toJson(QJsonDocument::Compact), &m_error))
        return false; // nothing is held that the disk does not hold
    m_jobs = jobs;
    dropExpired();
    return true;
}

void AcceptedJobsFile::dropExpired() {
    const QDateTime oldest = m_now().addDays(-acceptedJobsKeepDays);
    m_jobs.removeIf([&](const Record& record) { return record.acceptedAt <= oldest; });
}

} // namespace ketplus::runner::app

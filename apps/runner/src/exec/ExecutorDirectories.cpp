#include "exec/ExecutorDirectories.h"
#include "exec/ExecutionPlan.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QUuid>

namespace ketplus::runner::exec {
namespace {

const QString identityFile = QStringLiteral("identity.json");
constexpr QFile::Permissions ownerOnly = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
constexpr QFile::Permissions groupOrOther = QFile::ReadGroup | QFile::WriteGroup | QFile::ExeGroup |
                                            QFile::ReadOther | QFile::WriteOther | QFile::ExeOther;

bool validName(const QString& name) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{32}-[1-9][0-9]{0,17}$"));
    return pattern.match(name).hasMatch();
}

bool ownerOnlyDirectory(const QString& path) {
    const QFileInfo info(path);
    return info.isDir() && !info.isSymLink() && !(info.permissions() & groupOrOther);
}

} // namespace

ExecutorDirectories::ExecutorDirectories(QString root) : m_root(QDir::cleanPath(std::move(root))) {}

QString ExecutorDirectories::canonicalRoot() const {
    if (!QDir::isAbsolutePath(m_root))
        return {};
    if (!QFileInfo::exists(m_root) &&
        (!QDir().mkpath(m_root) || !QFile::setPermissions(m_root, ownerOnly)))
        return {};
    return ownerOnlyDirectory(m_root) ? QFileInfo(m_root).canonicalFilePath() : QString();
}

std::optional<ExecutorDirectories::Entry> ExecutorDirectories::create(const QString& jobId,
                                                                      const qint64 attempt) {
    const QString root = canonicalRoot();
    const QString taskId = executorTaskId(jobId, attempt);
    if (root.isEmpty() || !validName(taskId))
        return std::nullopt;
    const QString path = root + u'/' + taskId;
    if (!QDir(root).mkdir(taskId))
        return std::nullopt; // also when it exists: an existing job is never started again
    Entry entry{path, taskId, jobId, attempt, QUuid::createUuid().toString(QUuid::WithoutBraces)};
    const QByteArray identity = QJsonDocument(QJsonObject{{QStringLiteral("jobId"), jobId},
                                                          {QStringLiteral("attempt"), attempt},
                                                          {QStringLiteral("lease"), entry.lease}})
                                    .toJson(QJsonDocument::Compact);
    QSaveFile file(path + u'/' + identityFile);
    if (!QFile::setPermissions(path, ownerOnly) || !file.open(QIODevice::WriteOnly) ||
        file.write(identity) != identity.size() || !file.commit()) {
        QDir(path).removeRecursively();
        return std::nullopt;
    }
    return entry;
}

QStringList ExecutorDirectories::names() const {
    const QString root = canonicalRoot();
    if (root.isEmpty())
        return {};
    return QDir(root).entryList(
        QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot, QDir::Name);
}

std::optional<ExecutorDirectories::Entry> ExecutorDirectories::read(const QString& name) const {
    const QString root = canonicalRoot();
    const QString path = root + u'/' + name;
    if (root.isEmpty() || !validName(name) || !ownerOnlyDirectory(path) ||
        QFileInfo(path).canonicalFilePath() != path)
        return std::nullopt;
    QFile file(path + u'/' + identityFile);
    if (QFileInfo(file).isSymLink() || file.size() > 4096 || !file.open(QIODevice::ReadOnly))
        return std::nullopt;
    const QJsonObject identity = QJsonDocument::fromJson(file.readAll()).object();
    Entry entry{path, name, identity.value(QStringLiteral("jobId")).toString(),
                identity.value(QStringLiteral("attempt")).toInteger(),
                identity.value(QStringLiteral("lease")).toString()};
    if (QUuid::fromString(entry.lease).isNull() ||
        executorTaskId(entry.jobId, entry.attempt) != name)
        return std::nullopt;
    return entry;
}

bool ExecutorDirectories::remove(const Entry& entry) const {
    const QString root = canonicalRoot();
    if (root.isEmpty() || !validName(entry.taskId) || entry.path != root + u'/' + entry.taskId ||
        QFileInfo(entry.path).isSymLink())
        return false;
    return QDir(entry.path).removeRecursively();
}

} // namespace ketplus::runner::exec

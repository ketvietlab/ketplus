#include "snapshot/ReceiverStore.h"

#include "protocol/FrameRules.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

namespace ketplus::runner::receiver {
namespace {

constexpr int version = 1;
const QString repos = QStringLiteral("repos");
const QString jsonSuffix = QStringLiteral(".json");

QString repositoryName(const QString& key) {
    return repos + QLatin1Char('/') + key + QStringLiteral("/repository.json");
}
QString workspaceName(const QString& repositoryKey, const QString& key) {
    return repos + QLatin1Char('/') + repositoryKey + QStringLiteral("/workspaces/") + key +
           jsonSuffix;
}
QString partialName(const QString& repositoryKey, const QString& key) {
    return repos + QLatin1Char('/') + repositoryKey + QStringLiteral("/partial/") + key +
           jsonSuffix;
}

QString time(const QDateTime& when) { return when.toUTC().toString(Qt::ISODateWithMs); }

std::optional<QJsonObject> parse(const app::StateDirectory::ReadResult& read) {
    if (read.status != app::StateDirectory::ReadResult::Status::Ok)
        return std::nullopt;
    const QJsonDocument document = QJsonDocument::fromJson(read.data);
    if (!document.isObject() ||
        document.object().value(QStringLiteral("version")).toInt() != version)
        return std::nullopt;
    return document.object();
}

std::optional<QDateTime> timeOf(const QJsonObject& object, const QString& field) {
    const QDateTime when = QDateTime::fromString(object.value(field).toString(), Qt::ISODateWithMs);
    if (!when.isValid())
        return std::nullopt;
    return when.toUTC();
}

bool isSize(const QJsonValue& value) { return value.isDouble() && value.toDouble() >= 0; }

bool save(const app::StateDirectory& state, const QString& name, QJsonObject object,
          QString* error) {
    object.insert(QStringLiteral("version"), version);
    return state.write(name, QJsonDocument(object).toJson(QJsonDocument::Compact), error);
}

} // namespace

ReceiverStore::ReceiverStore(const app::StateDirectory& state, QString workRoot)
    : m_state(state), m_workRoot(QDir::cleanPath(std::move(workRoot))) {}

bool ReceiverStore::isKey(const QString& key) { return protocol::rules::isKey(key); }

QString ReceiverStore::repositoryDirectory(const QString& repositoryKey) const {
    return m_state.path() + QLatin1Char('/') + repos + QLatin1Char('/') + repositoryKey;
}

QString ReceiverStore::mirrorPath(const QString& repositoryKey) const {
    return repositoryDirectory(repositoryKey) + QStringLiteral("/mirror.git");
}

QString ReceiverStore::workspacePath(const QString& repositoryKey,
                                     const QString& workspaceKey) const {
    return m_workRoot + QLatin1Char('/') + repositoryKey + QLatin1Char('/') + workspaceKey;
}

QString ReceiverStore::partialPackPath(const QString& repositoryKey,
                                       const QString& workspaceKey) const {
    return repositoryDirectory(repositoryKey) + QStringLiteral("/partial/") + workspaceKey +
           QStringLiteral(".pack");
}

StoredRecords ReceiverStore::load() const {
    StoredRecords records;
    const QStringList keys = QDir(m_state.path() + QLatin1Char('/') + repos)
                                 .entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString& key : keys) {
        if (!isKey(key))
            continue;
        const std::optional<RepositoryRecord> repository = readRepository(key);
        if (!repository) {
            records.orphanRepositories.append(key);
            continue;
        }
        records.repositories.append(*repository);
        const QString base = repos + QLatin1Char('/') + key;
        QString error;
        for (const QString& name :
             m_state.list(base + QStringLiteral("/workspaces"), &error).value_or(QStringList{}))
            if (const auto workspace = readWorkspace(key, name))
                records.workspaces.append(*workspace);
        for (const QString& name :
             m_state.list(base + QStringLiteral("/partial"), &error).value_or(QStringList{}))
            if (const auto partial = readPartial(key, name))
                records.partials.append(*partial);
    }
    return records;
}

std::optional<RepositoryRecord> ReceiverStore::readRepository(const QString& key) const {
    const auto object = parse(m_state.read(repositoryName(key)));
    if (!object || !isSize(object->value(QStringLiteral("mirrorBytes"))))
        return std::nullopt;
    RepositoryRecord record{key, object->value(QStringLiteral("owner")).toString(),
                            qint64(object->value(QStringLiteral("mirrorBytes")).toDouble())};
    if (record.owner.isEmpty())
        return std::nullopt;
    return record;
}

std::optional<WorkspaceRecord> ReceiverStore::readWorkspace(const QString& repositoryKey,
                                                            const QString& name) const {
    if (!name.endsWith(jsonSuffix))
        return std::nullopt;
    const QString key = name.chopped(jsonSuffix.size());
    if (!isKey(key))
        return std::nullopt;
    const auto object = parse(m_state.read(workspaceName(repositoryKey, key)));
    if (!object)
        return std::nullopt;
    const auto lastUsed = timeOf(*object, QStringLiteral("lastUsed"));
    WorkspaceRecord record;
    record.repositoryKey = repositoryKey;
    record.workspaceKey = key;
    record.owner = object->value(QStringLiteral("owner")).toString();
    record.snapshotId = object->value(QStringLiteral("snapshotId")).toString();
    record.complete = object->value(QStringLiteral("complete")).toBool();
    record.bytes = qint64(object->value(QStringLiteral("bytes")).toDouble());
    if (!lastUsed || record.owner.isEmpty() || !isSize(object->value(QStringLiteral("bytes"))) ||
        (record.complete && !protocol::rules::isObjectId(record.snapshotId)))
        return std::nullopt;
    record.lastUsed = *lastUsed;
    return record;
}

std::optional<PartialRecord> ReceiverStore::readPartial(const QString& repositoryKey,
                                                        const QString& name) const {
    if (!name.endsWith(jsonSuffix))
        return std::nullopt;
    const QString key = name.chopped(jsonSuffix.size());
    if (!isKey(key))
        return std::nullopt;
    const auto object = parse(m_state.read(partialName(repositoryKey, key)));
    if (!object)
        return std::nullopt;
    const auto lastWrite = timeOf(*object, QStringLiteral("lastWrite"));
    PartialRecord record;
    record.repositoryKey = repositoryKey;
    record.workspaceKey = key;
    record.packSha256 = object->value(QStringLiteral("packSha256")).toString();
    record.packBytes = qint64(object->value(QStringLiteral("packBytes")).toDouble());
    record.offset = qint64(object->value(QStringLiteral("offset")).toDouble());
    if (!lastWrite || !protocol::rules::isLowerHex(record.packSha256, 64) ||
        !isSize(object->value(QStringLiteral("offset"))) || record.offset > record.packBytes)
        return std::nullopt;
    record.lastWrite = *lastWrite;
    return record;
}

bool ReceiverStore::write(const RepositoryRecord& record, QString* error) const {
    return save(m_state, repositoryName(record.repositoryKey),
                {{QStringLiteral("owner"), record.owner},
                 {QStringLiteral("mirrorBytes"), double(record.mirrorBytes)}},
                error);
}

bool ReceiverStore::write(const WorkspaceRecord& record, QString* error) const {
    return save(m_state, workspaceName(record.repositoryKey, record.workspaceKey),
                {{QStringLiteral("owner"), record.owner},
                 {QStringLiteral("snapshotId"), record.snapshotId},
                 {QStringLiteral("complete"), record.complete},
                 {QStringLiteral("lastUsed"), time(record.lastUsed)},
                 {QStringLiteral("bytes"), double(record.bytes)}},
                error);
}

bool ReceiverStore::write(const PartialRecord& record, QString* error) const {
    return save(m_state, partialName(record.repositoryKey, record.workspaceKey),
                {{QStringLiteral("packSha256"), record.packSha256},
                 {QStringLiteral("packBytes"), double(record.packBytes)},
                 {QStringLiteral("offset"), double(record.offset)},
                 {QStringLiteral("lastWrite"), time(record.lastWrite)}},
                error);
}

bool ReceiverStore::removeWorkspaceRecord(const QString& repositoryKey,
                                          const QString& workspaceKey) const {
    QString error;
    return m_state.remove(workspaceName(repositoryKey, workspaceKey), &error);
}

bool ReceiverStore::removePartial(const QString& repositoryKey, const QString& workspaceKey) const {
    QString error;
    const bool pack = !QFileInfo::exists(partialPackPath(repositoryKey, workspaceKey)) ||
                      QFile::remove(partialPackPath(repositoryKey, workspaceKey));
    return m_state.remove(partialName(repositoryKey, workspaceKey), &error) && pack;
}

bool ReceiverStore::removeRepository(const QString& repositoryKey) const {
    bool ok = QDir(repositoryDirectory(repositoryKey)).removeRecursively();
    const QString work = m_workRoot + QLatin1Char('/') + repositoryKey;
    if (QFileInfo::exists(work))
        ok = QDir(work).removeRecursively() && ok;
    return ok;
}

} // namespace ketplus::runner::receiver

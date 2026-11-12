#include "ssh/SshMachineStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

#include <utility>

namespace ketplus::remote::ssh {

namespace {

constexpr int storeVersion = 1;

bool fail(QString* error, const QString& message) {
    if (error)
        *error = message;
    return false;
}

} // namespace

SshTarget parseSshDestination(const QString& input, QString* error) {
    QString rest = input.trimmed();
    SshTarget target;
    const qsizetype at = rest.lastIndexOf('@');
    if (at >= 0) {
        target.user = rest.left(at);
        rest = rest.mid(at + 1);
    }
    QString port;
    if (rest.startsWith('[')) {
        const qsizetype close = rest.indexOf(']');
        if (close < 0) {
            fail(error, QStringLiteral("Close the bracket around the host address."));
            return {};
        }
        const QString after = rest.mid(close + 1);
        if (!after.isEmpty() && !after.startsWith(':')) {
            fail(error, QStringLiteral("Enter user@host or user@host:port."));
            return {};
        }
        port = after.mid(1);
        rest = rest.mid(1, close - 1);
    } else if (rest.count(':') == 1) {
        port = rest.section(':', 1);
        rest = rest.section(':', 0, 0);
    }
    target.alias = rest;
    if (!port.isEmpty()) {
        bool ok = false;
        target.port = port.toInt(&ok);
        if (!ok || target.port < 1 || target.port > 65535) {
            fail(error, QStringLiteral("The port must be a number from 1 to 65535."));
            return {};
        }
    }
    if ((at >= 0 && target.user.isEmpty()) || !target.isValid()) {
        fail(error, QStringLiteral("Enter user@host or user@host:port."));
        return {};
    }
    return target;
}

QString sshTargetKey(const SshTarget& target) {
    QString key = target.alias;
    if (!target.user.isEmpty())
        key.prepend(target.user + '@');
    if (target.port > 0)
        key += ':' + QString::number(target.port);
    return key;
}

QString SshMachineStore::defaultPath() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           QStringLiteral("/Ketsuite/KetPlus/ssh/machines.json");
}

SshMachineStore::SshMachineStore(QString path) : path_(std::move(path)) { load(); }

void SshMachineStore::load() {
    QFile file(path_);
    if (!file.exists())
        return;
    if (!file.open(QIODevice::ReadOnly)) {
        usable_ = false;
        return;
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    const QJsonObject root = document.object();
    if (!document.isObject() || root.value(QStringLiteral("version")).toInt() != storeVersion) {
        // Never replace a file this build does not understand.
        usable_ = false;
        return;
    }
    for (const QJsonValue& value : root.value(QStringLiteral("machines")).toArray()) {
        const QJsonObject machine = value.toObject();
        SshMachineRecord record{
            SshTarget::fromJson(machine.value(QStringLiteral("target")).toObject()),
            machine.value(QStringLiteral("machine_id")).toString(),
            {}};
        for (const QJsonValue& name : machine.value(QStringLiteral("accepted_host_keys")).toArray())
            if (name.isString() && !name.toString().isEmpty() &&
                !record.acceptedHostKeys.contains(name.toString()))
                record.acceptedHostKeys.append(name.toString());
        if (record.target.isValid() && indexOf(sshTargetKey(record.target)) < 0)
            machines_.append(std::move(record));
    }
    const QJsonObject bindings = root.value(QStringLiteral("bindings")).toObject();
    for (auto it = bindings.begin(); it != bindings.end(); ++it)
        if (it.value().isString())
            bindings_.insert(it.key(), it.value().toString());
}

bool SshMachineStore::save(QString* error) {
    if (!usable_)
        return fail(error, QStringLiteral("The saved SSH machines could not be read."));
    QJsonArray machines;
    for (const SshMachineRecord& record : std::as_const(machines_)) {
        QJsonObject machine{{QStringLiteral("target"), record.target.toJson()}};
        if (!record.machineId.isEmpty())
            machine.insert(QStringLiteral("machine_id"), record.machineId);
        if (!record.acceptedHostKeys.isEmpty())
            machine.insert(QStringLiteral("accepted_host_keys"),
                           QJsonArray::fromStringList(record.acceptedHostKeys));
        machines.append(machine);
    }
    QJsonObject bindings;
    for (auto it = bindings_.cbegin(); it != bindings_.cend(); ++it)
        bindings.insert(it.key(), it.value());
    const QJsonObject root{{QStringLiteral("version"), storeVersion},
                           {QStringLiteral("machines"), machines},
                           {QStringLiteral("bindings"), bindings}};

    const QString directory = QFileInfo(path_).absolutePath();
    if (!QDir().mkpath(directory))
        return fail(error, QStringLiteral("The SSH machines folder could not be created."));
    QFile::setPermissions(directory,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    QSaveFile file(path_);
    if (!file.open(QIODevice::WriteOnly))
        return fail(error, QStringLiteral("The SSH machines could not be saved."));
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!file.commit())
        return fail(error, QStringLiteral("The SSH machines could not be saved."));
    return true;
}

qsizetype SshMachineStore::indexOf(const QString& targetKey) const {
    for (qsizetype index = 0; index < machines_.size(); ++index)
        if (sshTargetKey(machines_.at(index).target) == targetKey)
            return index;
    return -1;
}

bool SshMachineStore::contains(const QString& targetKey) const { return indexOf(targetKey) >= 0; }

QString SshMachineStore::machineId(const QString& targetKey) const {
    const qsizetype index = indexOf(targetKey);
    return index < 0 ? QString() : machines_.at(index).machineId;
}

bool SshMachineStore::add(const SshTarget& target, QString* error) {
    if (!target.isValid())
        return fail(error, QStringLiteral("Enter user@host or user@host:port."));
    if (contains(sshTargetKey(target)))
        return true;
    machines_.append({target, {}, {}});
    if (save(error))
        return true;
    machines_.removeLast();
    return false;
}

bool SshMachineStore::remove(const QString& targetKey, QString* error) {
    const qsizetype index = indexOf(targetKey);
    if (index < 0)
        return true;
    const auto machines = machines_;
    const auto bindings = bindings_;
    const QString machineId = machines_.takeAt(index).machineId;
    // Another target can reach the same daemon (an alias and its address); its bindings stay.
    bool shared = false;
    for (const SshMachineRecord& record : std::as_const(machines_))
        shared = shared || (!machineId.isEmpty() && record.machineId == machineId);
    if (!machineId.isEmpty() && !shared)
        bindings_.removeIf([&machineId](const auto& entry) { return entry.value() == machineId; });
    if (save(error))
        return true;
    machines_ = machines;
    bindings_ = bindings;
    return false;
}

bool SshMachineStore::trustFirstIdentity(const QString& targetKey, const QString& machineId,
                                         QString* error) {
    const qsizetype index = indexOf(targetKey);
    if (index < 0)
        return fail(error, QStringLiteral("This SSH machine is not saved."));
    if (machineId.isEmpty())
        return fail(error, QStringLiteral("The machine named no identity."));
    if (machines_.at(index).machineId == machineId)
        return true;
    if (!machines_.at(index).machineId.isEmpty())
        return fail(error, QStringLiteral("This machine already has an identity."));
    machines_[index].machineId = machineId;
    if (save(error))
        return true;
    machines_[index].machineId.clear();
    return false;
}

bool SshMachineStore::retrust(const QString& targetKey, const QString& machineId, QString* error) {
    const qsizetype index = indexOf(targetKey);
    if (index < 0)
        return fail(error, QStringLiteral("This SSH machine is not saved."));
    if (machineId.isEmpty())
        return fail(error, QStringLiteral("The machine named no identity."));
    const auto machines = machines_;
    const auto bindings = bindings_;
    const QString previous = std::exchange(machines_[index].machineId, machineId);
    if (!previous.isEmpty() && previous != machineId) {
        for (auto it = bindings_.begin(); it != bindings_.end(); ++it)
            if (it.value() == previous)
                it.value() = machineId;
        // Every saved route to the old daemon now names the new one.
        for (SshMachineRecord& record : machines_)
            if (record.machineId == previous)
                record.machineId = machineId;
    }
    if (save(error))
        return true;
    machines_ = machines;
    bindings_ = bindings;
    return false;
}

bool SshMachineStore::recordAcceptedHostKey(const QString& targetKey, const QString& knownHostsName,
                                            QString* error) {
    const qsizetype index = indexOf(targetKey);
    if (index < 0)
        return fail(error, QStringLiteral("This SSH machine is not saved."));
    if (knownHostsName.isEmpty() || machines_.at(index).acceptedHostKeys.contains(knownHostsName))
        return true;
    machines_[index].acceptedHostKeys.append(knownHostsName);
    if (save(error))
        return true;
    machines_[index].acceptedHostKeys.removeLast();
    return false;
}

QStringList SshMachineStore::acceptedHostKeys(const QString& targetKey) const {
    const qsizetype index = indexOf(targetKey);
    return index < 0 ? QStringList() : machines_.at(index).acceptedHostKeys;
}

QStringList SshMachineStore::releasedHostKeys(const QString& targetKey) const {
    const qsizetype index = indexOf(targetKey);
    if (index < 0)
        return {};
    QStringList released;
    for (const QString& name : machines_.at(index).acceptedHostKeys) {
        bool shared = false;
        for (qsizetype other = 0; other < machines_.size() && !shared; ++other)
            shared = other != index && machines_.at(other).acceptedHostKeys.contains(name);
        if (!shared)
            released.append(name);
    }
    return released;
}

bool SshMachineStore::bindConversation(const QString& conversationId, const QString& machineId,
                                       QString* error) {
    if (conversationId.isEmpty() || machineId.isEmpty())
        return fail(error,
                    QStringLiteral("A conversation binding needs a conversation and a machine."));
    if (bindings_.value(conversationId) == machineId)
        return true;
    const auto bindings = bindings_;
    bindings_.insert(conversationId, machineId);
    if (save(error))
        return true;
    bindings_ = bindings;
    return false;
}

QString SshMachineStore::conversationMachine(const QString& conversationId) const {
    return bindings_.value(conversationId);
}

} // namespace ketplus::remote::ssh

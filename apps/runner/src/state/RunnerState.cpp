#include "state/RunnerState.h"

#include "RunnerRules.h"
#include "cloud/CryptoPrimitives.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <openssl/evp.h>

namespace ketplus::runner::app {
namespace {

const QString keyFile = QStringLiteral("runner.key");
const QString credentialFile = QStringLiteral("credential.json");
const QString configFile = QStringLiteral("config.json");
const QString pinDirectory = QStringLiteral("pins");
constexpr int recordVersion = 1;

QString pinFile(const QString& machineId) {
    return pinDirectory + QLatin1Char('/') + machineId + QStringLiteral(".json");
}

QByteArray compact(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

// The object of a record file, or nothing with `error` set. The message names the file only.
std::optional<QJsonObject> readRecord(const StateDirectory& directory, const QString& name,
                                      QString* error) {
    const StateDirectory::ReadResult read = directory.read(name);
    if (read.status == StateDirectory::ReadResult::Status::Invalid) {
        *error = read.error;
        return std::nullopt;
    }
    const QString path = directory.path() + QLatin1Char('/') + name;
    if (read.status == StateDirectory::ReadResult::Status::Missing) {
        *error = QStringLiteral("%1 is missing").arg(path);
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(read.data);
    if (!document.isObject() ||
        document.object().value(QStringLiteral("version")).toInteger(-1) != recordVersion) {
        *error = QStringLiteral("%1 is corrupt; it was left as it is").arg(path);
        return std::nullopt;
    }
    return document.object();
}

QString corrupt(const StateDirectory& directory, const QString& name) {
    return QStringLiteral("%1/%2 is corrupt; it was left as it is").arg(directory.path(), name);
}

} // namespace

bool RunnerState::hasIdentity() const {
    return m_directory.exists(keyFile) || m_directory.exists(credentialFile) ||
           m_directory.exists(configFile);
}

std::optional<RunnerIdentity> RunnerState::loadIdentity(QString* error) const {
    if (!hasIdentity()) {
        *error = QStringLiteral("This runner is not registered; run `ketplus-runner register`");
        return std::nullopt;
    }
    RunnerIdentity identity;
    const StateDirectory::ReadResult key = m_directory.read(keyFile, 64);
    if (key.status != StateDirectory::ReadResult::Status::Ok) {
        *error = key.status == StateDirectory::ReadResult::Status::Missing
                     ? QStringLiteral("%1/%2 is missing").arg(m_directory.path(), keyFile)
                     : key.error;
        return std::nullopt;
    }
    identity.privateKey = key.data;
    identity.publicKey = control_crypto::publicKey(identity.privateKey, EVP_PKEY_ED25519);
    if (identity.privateKey.size() != 32 || identity.publicKey.size() != 32) {
        *error = corrupt(m_directory, keyFile);
        return std::nullopt;
    }

    const std::optional<QJsonObject> credential = readRecord(m_directory, credentialFile, error);
    if (!credential)
        return std::nullopt;
    RunnerCredential& c = identity.credential;
    c.runnerId = credential->value(QStringLiteral("runnerId")).toString();
    c.workspaceId = credential->value(QStringLiteral("workspaceId")).toString();
    c.keyVersion = credential->value(QStringLiteral("keyVersion")).toInt(0);
    c.refreshCredential = credential->value(QStringLiteral("refreshCredential")).toString();
    if (!isRelayId(c.runnerId) || !isRelayId(c.workspaceId) || c.keyVersion < 1 ||
        c.refreshCredential.size() < 32 || c.refreshCredential.size() > 512) {
        *error = corrupt(m_directory, credentialFile);
        return std::nullopt;
    }

    const std::optional<QJsonObject> config = readRecord(m_directory, configFile, error);
    if (!config)
        return std::nullopt;
    const std::optional<QString> name =
        normalizedName(config->value(QStringLiteral("name")).toString());
    const QJsonValue labels = config->value(QStringLiteral("labels"));
    bool labelsAreStrings = labels.isArray();
    for (const QJsonValue& label : labels.toArray()) {
        labelsAreStrings = labelsAreStrings && label.isString();
        identity.config.labels.append(label.toString());
    }
    if (!name || !labelsAreStrings || !labelsError(identity.config.labels).isEmpty()) {
        *error = corrupt(m_directory, configFile);
        return std::nullopt;
    }
    identity.config.name = *name;
    return identity;
}

bool RunnerState::saveKey(const QByteArray& privateKey, QString* error) const {
    return m_directory.write(keyFile, privateKey, error);
}

bool RunnerState::saveConfig(const RunnerConfig& config, QString* error) const {
    return m_directory.write(
        configFile,
        compact({{QStringLiteral("version"), recordVersion},
                 {QStringLiteral("name"), config.name},
                 {QStringLiteral("labels"), QJsonArray::fromStringList(config.labels)}}),
        error);
}

bool RunnerState::saveCredential(const RunnerCredential& credential, QString* error) const {
    return m_directory.write(
        credentialFile,
        compact({{QStringLiteral("version"), recordVersion},
                 {QStringLiteral("runnerId"), credential.runnerId},
                 {QStringLiteral("workspaceId"), credential.workspaceId},
                 {QStringLiteral("keyVersion"), credential.keyVersion},
                 {QStringLiteral("refreshCredential"), credential.refreshCredential}}),
        error);
}

bool RunnerState::removeIdentity(QString* error) const {
    const std::optional<QStringList> pins = m_directory.list(pinDirectory, error);
    if (!pins)
        return false;
    for (const QString& pin : *pins) {
        if (!m_directory.remove(pinDirectory + QLatin1Char('/') + pin, error))
            return false;
    }
    // The credential goes first: a key left alone is still refused by `register` and is
    // cleared by `deregister --local-only`.
    return m_directory.remove(credentialFile, error) && m_directory.remove(configFile, error) &&
           m_directory.remove(keyFile, error);
}

std::optional<QList<pairing::RunnerPin>> RunnerState::loadPins(QString* error) const {
    const std::optional<QStringList> names = m_directory.list(pinDirectory, error);
    if (!names)
        return std::nullopt;
    QList<pairing::RunnerPin> pins;
    for (const QString& name : *names) {
        const QString file = pinDirectory + QLatin1Char('/') + name;
        const std::optional<QJsonObject> record = readRecord(m_directory, file, error);
        if (!record)
            return std::nullopt;
        const pairing::PairingResult<pairing::RunnerPin> pin =
            pairing::RunnerPin::fromJson(*record);
        if (!pin || pinFile(pin.value().machineId) != file) {
            *error = corrupt(m_directory, file);
            return std::nullopt;
        }
        pins.append(pin.value());
    }
    return pins;
}

bool RunnerState::savePin(const pairing::RunnerPin& pin, QString* error) const {
    if (!isRelayId(pin.machineId)) {
        *error = QStringLiteral("The desktop's machine id is not valid");
        return false;
    }
    return m_directory.write(pinFile(pin.machineId), compact(pin.toJson()), error);
}

bool RunnerState::removePin(const QString& machineId, QString* error) const {
    if (!isRelayId(machineId)) {
        *error = QStringLiteral("The machine id is not valid");
        return false;
    }
    return m_directory.remove(pinFile(machineId), error);
}

} // namespace ketplus::runner::app

#include "runner/RunnerPinStore.h"

#include "cloud/CredentialStore.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace ketplus::daemon::runner {
namespace {

using ketplus::runner::pairing::DesktopRunnerPin;

constexpr int recordVersion = 1;

const QString& account() {
    static const QString name = QString::fromLatin1(runnerPinsAccount);
    return name;
}

enum class Record { Missing, Current, Corrupt, OtherVersion };

Record readRecord(const QByteArray& bytes, QJsonObject* runners) {
    if (bytes.isEmpty())
        return Record::Missing;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return Record::Corrupt;
    const QJsonObject object = document.object();
    const QJsonValue version = object.value(QStringLiteral("version"));
    if (!version.isDouble())
        return Record::Corrupt;
    if (version.toDouble() != recordVersion)
        return Record::OtherVersion;
    const QJsonValue list = object.value(QStringLiteral("runners"));
    if (!list.isObject())
        return Record::Corrupt;
    *runners = list.toObject();
    return Record::Current;
}

QByteArray encode(const QJsonObject& runners) {
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), recordVersion},
                                     {QStringLiteral("runners"), runners}})
        .toJson(QJsonDocument::Compact);
}

std::optional<DesktopRunnerPin> readPin(const QString& runnerId, const QJsonValue& value) {
    if (!value.isObject())
        return std::nullopt;
    const auto pin = DesktopRunnerPin::fromJson(value.toObject());
    if (!pin || pin.value().runnerId != runnerId)
        return std::nullopt;
    return pin.value();
}

} // namespace

RunnerPinStore::Loaded RunnerPinStore::load() const {
    Loaded loaded;
    QString error;
    const QByteArray bytes = store_.read(account(), &error);
    if (!error.isEmpty()) {
        loaded.error = QStringLiteral("Runner pins could not be read.");
        return loaded;
    }
    QJsonObject runners;
    switch (readRecord(bytes, &runners)) {
    case Record::Missing:
        return loaded;
    case Record::Corrupt:
        loaded.error = QStringLiteral("Runner pins are corrupt; no runner is trusted.");
        return loaded;
    case Record::OtherVersion:
        loaded.error = QStringLiteral("Runner pins have another version; no runner is trusted.");
        return loaded;
    case Record::Current:
        break;
    }
    for (auto it = runners.constBegin(); it != runners.constEnd(); ++it) {
        if (auto pin = readPin(it.key(), it.value()))
            loaded.pins.insert(it.key(), std::move(*pin));
        else
            loaded.ignored.append(it.key());
    }
    return loaded;
}

std::optional<DesktopRunnerPin> RunnerPinStore::pin(const QString& runnerId) const {
    const Loaded loaded = load();
    const auto it = loaded.pins.constFind(runnerId);
    if (it == loaded.pins.cend())
        return std::nullopt;
    return *it;
}

bool RunnerPinStore::store(const DesktopRunnerPin& pin, QString* error) {
    // The record must read back before it is trusted.
    const QJsonObject record = pin.toJson();
    if (!readPin(pin.runnerId, record)) {
        if (error)
            *error = QStringLiteral("The runner pin is invalid.");
        return false;
    }
    bool refused = false;
    QString storeError;
    const bool stored = store_.update(
        [&](QHash<QString, QByteArray>& accounts) {
            QJsonObject runners;
            // A corrupt record trusts nothing and is replaced; one of another version is
            // kept for the build that wrote it.
            if (readRecord(accounts.value(account()), &runners) == Record::OtherVersion) {
                refused = true;
                return;
            }
            runners.insert(pin.runnerId, record);
            accounts.insert(account(), encode(runners));
        },
        &storeError);
    if (refused || !stored) {
        if (error)
            *error = refused ? QStringLiteral("Runner pins have another version.")
                             : QStringLiteral("The runner pin could not be stored.");
        return false;
    }
    return true;
}

bool RunnerPinStore::remove(const QString& runnerId, QString* error) {
    QString storeError;
    const bool removed = store_.update(
        [&](QHash<QString, QByteArray>& accounts) {
            QJsonObject runners;
            if (readRecord(accounts.value(account()), &runners) != Record::Current ||
                !runners.contains(runnerId))
                return;
            runners.remove(runnerId);
            accounts.insert(account(), encode(runners));
        },
        &storeError);
    if (!removed && error)
        *error = QStringLiteral("The runner pin could not be removed.");
    return removed;
}

} // namespace ketplus::daemon::runner

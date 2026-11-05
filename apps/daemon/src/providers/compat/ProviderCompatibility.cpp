#include "providers/compat/ProviderCompatibility.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

#include <algorithm>

namespace ketplus::daemon::compat {
namespace {

QJsonObject builtInManifest() {
    static const QJsonObject manifest = [] {
        QFile file(QStringLiteral(":/ketplus/daemon/provider-compatibility.json"));
        if (!file.open(QIODevice::ReadOnly))
            return QJsonObject{};
        return QJsonDocument::fromJson(file.readAll()).object();
    }();
    return manifest;
}

QJsonObject readJson(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.read(64 * 1024)).object();
}

} // namespace

QString statusName(const SupportStatus status) {
    switch (status) {
    case SupportStatus::Supported:
        return QStringLiteral("supported");
    case SupportStatus::Newer:
        return QStringLiteral("newer");
    case SupportStatus::Outdated:
        return QStringLiteral("outdated");
    case SupportStatus::Unknown:
        return QStringLiteral("unknown");
    case SupportStatus::Missing:
        return QStringLiteral("missing");
    case SupportStatus::Bundled:
        return QStringLiteral("bundled");
    }
    return QStringLiteral("unknown");
}

QString Version::toString() const {
    QStringList text;
    for (const int part : parts)
        text.append(QString::number(part));
    return text.join(QLatin1Char('.'));
}

Version parseVersion(const QString& text) {
    static const QRegularExpression dotted(QStringLiteral("(\\d+(?:\\.\\d+)+)"));
    const QRegularExpressionMatch match = dotted.match(text);
    if (!match.hasMatch())
        return {};
    Version version;
    for (const QString& part : match.captured(1).split(QLatin1Char('.'))) {
        bool ok = false;
        const int value = part.toInt(&ok);
        if (!ok)
            return {};
        version.parts.append(value);
    }
    return version;
}

int compare(const Version& left, const Version& right) {
    const qsizetype length = std::max(left.parts.size(), right.parts.size());
    for (qsizetype index = 0; index < length; ++index) {
        const int a = index < left.parts.size() ? left.parts.at(index) : 0;
        const int b = index < right.parts.size() ? right.parts.at(index) : 0;
        if (a != b)
            return a < b ? -1 : 1;
    }
    return 0;
}

QString Policy::updateCommandFor(const QString& executable) const {
    // Only brew-managed installs live under Cellar/Caskroom; an npm global installed with
    // Homebrew's node also sits under /opt/homebrew, but resolves into lib/node_modules.
    const QString canonical = QFileInfo(executable).canonicalFilePath();
    const QString resolved = canonical.isEmpty() ? executable : canonical;
    const bool homebrew = resolved.contains(QStringLiteral("/Cellar/")) ||
                          resolved.contains(QStringLiteral("/Caskroom/"));
    return homebrew && !homebrewUpdateCommand.isEmpty() ? homebrewUpdateCommand : updateCommand;
}

const Dialect* Policy::dialectFor(const Version& version) const {
    if (dialects.isEmpty())
        return nullptr;
    if (!version.isValid())
        return &dialects.last();
    const Dialect* match = nullptr;
    for (const Dialect& dialect : dialects) {
        if (compare(version, dialect.from) >= 0)
            match = &dialect;
    }
    return match;
}

bool Policy::baselineSupports(const QString& capability) const {
    if (bundled)
        return capabilities.contains(capability);
    return dialects.isEmpty() || dialects.last().capabilities.contains(capability);
}

namespace {

QStringList sortedStrings(const QJsonArray& values) {
    QStringList result;
    for (const QJsonValue& value : values) {
        if (value.isString() && !result.contains(value.toString()))
            result.append(value.toString());
    }
    result.sort();
    return result;
}

} // namespace

Policy policyFrom(const QJsonObject& manifest, const QString& provider) {
    const QJsonObject entry =
        manifest.value(QStringLiteral("providers")).toObject().value(provider).toObject();
    Policy result;
    result.provider = provider;
    result.name = entry.value(QStringLiteral("name")).toString(provider);
    result.minimum = parseVersion(entry.value(QStringLiteral("minimum")).toString());
    for (const QJsonValue& value : entry.value(QStringLiteral("tested")).toArray()) {
        const Version tested = parseVersion(value.toString());
        if (!tested.isValid())
            continue;
        result.tested.append(tested.toString());
        if (!result.newestTested.isValid() || compare(tested, result.newestTested) > 0)
            result.newestTested = tested;
    }
    result.updateCommand = entry.value(QStringLiteral("update")).toString();
    result.homebrewUpdateCommand = entry.value(QStringLiteral("update_homebrew")).toString();
    result.bundled = entry.value(QStringLiteral("bundled")).toBool();
    result.capabilities = sortedStrings(entry.value(QStringLiteral("capabilities")).toArray());
    for (const QJsonValue& value : entry.value(QStringLiteral("dialects")).toArray()) {
        const QJsonObject source = value.toObject();
        Dialect dialect{source.value(QStringLiteral("id")).toString(),
                        parseVersion(source.value(QStringLiteral("from")).toString()),
                        sortedStrings(source.value(QStringLiteral("capabilities")).toArray())};
        if (!dialect.id.isEmpty() && dialect.from.isValid())
            result.dialects.append(dialect);
    }
    std::sort(result.dialects.begin(), result.dialects.end(),
              [](const Dialect& left, const Dialect& right) {
                  return compare(left.from, right.from) < 0;
              });
    return result;
}

Policy policy(const QString& provider) { return policyFrom(builtInManifest(), provider); }

QJsonObject Runtime::toJson() const {
    QJsonObject json{{QStringLiteral("provider"), provider},
                     {QStringLiteral("name"), name},
                     {QStringLiteral("status"), statusName(status)}};
    const auto insert = [&json](const QString& key, const QString& value) {
        if (!value.isEmpty())
            json.insert(key, value);
    };
    insert(QStringLiteral("executable"), executable);
    insert(QStringLiteral("version"), version);
    insert(QStringLiteral("minimum"), minimum);
    insert(QStringLiteral("newest_tested"), newestTested);
    insert(QStringLiteral("update_command"), updateCommand);
    insert(QStringLiteral("message"), message);
    if (!components.isEmpty())
        json.insert(QStringLiteral("components"), components);
    insert(QStringLiteral("dialect"), dialect);
    json.insert(QStringLiteral("capabilities"), QJsonArray::fromStringList(capabilities));
    return json;
}

Runtime evaluate(const Policy& policy, const QString& executable, const QString& versionOutput) {
    Runtime runtime;
    runtime.provider = policy.provider;
    runtime.name = policy.name;
    runtime.executable = executable;
    runtime.minimum = policy.minimum.toString();
    runtime.newestTested = policy.newestTested.toString();
    runtime.updateCommand = policy.updateCommandFor(executable);
    const Version version = parseVersion(versionOutput);
    // Outdated CLIs never start, so they get no dialect and no capabilities.
    const bool outdated =
        version.isValid() && policy.isValid() && compare(version, policy.minimum) < 0;
    if (const Dialect* dialect = outdated ? nullptr : policy.dialectFor(version)) {
        runtime.dialect = dialect->id;
        runtime.capabilities = dialect->capabilities;
    }
    if (!version.isValid() || !policy.isValid()) {
        runtime.status = SupportStatus::Unknown;
        runtime.message = QStringLiteral("KetPlus could not read the %1 version, so it runs "
                                         "without a compatibility check.")
                              .arg(policy.name);
        return runtime;
    }
    runtime.version = version.toString();
    if (compare(version, policy.minimum) < 0) {
        runtime.status = SupportStatus::Outdated;
        runtime.message =
            QStringLiteral("%1 %2 is older than the oldest version KetPlus supports (%3). "
                           "Update it with `%4`, then send the message again.")
                .arg(policy.name, runtime.version, runtime.minimum, runtime.updateCommand);
    } else if (policy.newestTested.isValid() && compare(version, policy.newestTested) > 0) {
        runtime.status = SupportStatus::Newer;
        runtime.message = QStringLiteral("%1 %2 is newer than the newest version KetPlus has "
                                         "been tested with (%3). It should work; some features "
                                         "may not.")
                              .arg(policy.name, runtime.version, runtime.newestTested);
    } else {
        runtime.status = SupportStatus::Supported;
    }
    return runtime;
}

Runtime missing(const Policy& policy) {
    Runtime runtime;
    runtime.provider = policy.provider;
    runtime.name = policy.name;
    runtime.status = SupportStatus::Missing;
    runtime.minimum = policy.minimum.toString();
    runtime.newestTested = policy.newestTested.toString();
    runtime.message = QStringLiteral("%1 was not found.").arg(policy.name);
    return runtime;
}

Runtime bundledClaude(const QString& sidecarEntry) {
    return bundledClaude(sidecarEntry, policy(QStringLiteral("claude")));
}

Runtime bundledClaude(const QString& sidecarEntry, const Policy& policy) {
    Runtime runtime;
    runtime.provider = QStringLiteral("claude");
    runtime.name =
        policy.name == policy.provider ? QStringLiteral("Claude Agent SDK") : policy.name;
    if (sidecarEntry.isEmpty()) {
        runtime.status = SupportStatus::Missing;
        runtime.message = QStringLiteral("Claude Agent SDK sidecar was not found.");
        return runtime;
    }
    runtime.executable = sidecarEntry;
    runtime.status = SupportStatus::Bundled;
    runtime.dialect = QStringLiteral("bundled");
    runtime.capabilities = policy.capabilities;
    const QDir sidecar = QFileInfo(sidecarEntry).absoluteDir();
    const QJsonObject staged = readJson(sidecar.filePath(QStringLiteral("runtime.json")));
    // A source checkout has no runtime.json; the installed SDK package still names both versions.
    const QJsonObject sdk = readJson(sidecar.filePath(
        QStringLiteral("node_modules/@anthropic-ai/claude-agent-sdk/package.json")));
    const QString sdkVersion = staged.value(QStringLiteral("sdkVersion"))
                                   .toString(sdk.value(QStringLiteral("version")).toString());
    const QString claudeCode =
        staged.value(QStringLiteral("claudeCodeVersion"))
            .toString(sdk.value(QStringLiteral("claudeCodeVersion")).toString());
    runtime.version = sdkVersion;
    if (!sdkVersion.isEmpty())
        runtime.components.insert(QStringLiteral("sdk"), sdkVersion);
    if (!claudeCode.isEmpty())
        runtime.components.insert(QStringLiteral("claude_code"), claudeCode);
    return runtime;
}

} // namespace ketplus::daemon::compat

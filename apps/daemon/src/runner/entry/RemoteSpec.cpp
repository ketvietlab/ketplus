#include "runner/entry/RemoteSpec.h"

#include "protocol/FrameLimits.h"
#include "runner/sync/SnapshotPreparation.h"

#include <QJsonArray>
#include <QSet>

namespace ketplus::daemon::runner::entry {

namespace {

// The keys of a process execution after the shared checks.
const QSet<QString>& processKeys() {
    static const QSet<QString> keys{QStringLiteral("argv"), QStringLiteral("shell_command"),
                                    QStringLiteral("cwd"), QStringLiteral("env_delta")};
    return keys;
}

} // namespace

std::optional<protocol::JobSpec> jobSpecOf(const QJsonObject& execution, const qint64 timeoutMs) {
    for (auto it = execution.begin(); it != execution.end(); ++it)
        if (!processKeys().contains(it.key()))
            return std::nullopt;
    const bool hasArgv = execution.contains(QStringLiteral("argv"));
    if (hasArgv == execution.contains(QStringLiteral("shell_command")))
        return std::nullopt;
    protocol::JobSpec spec;
    if (hasArgv) {
        QStringList argv;
        for (const QJsonValue& argument : execution.value(QStringLiteral("argv")).toArray()) {
            if (!argument.isString())
                return std::nullopt;
            argv.append(argument.toString());
        }
        spec.invocation.argv = argv;
    } else {
        spec.invocation.shell = execution.value(QStringLiteral("shell_command")).toString();
    }
    const QJsonObject delta = execution.value(QStringLiteral("env_delta")).toObject();
    for (auto it = delta.begin(); it != delta.end(); ++it) {
        if (it.value().isNull())
            spec.env.insert(it.key(), std::nullopt);
        else if (it.value().isString())
            spec.env.insert(it.key(), it.value().toString());
        else
            return std::nullopt;
    }
    spec.cwd = execution.value(QStringLiteral("cwd")).toString();
    spec.timeoutMs = timeoutMs;
    return spec;
}

QString remoteSpecProblem(const QJsonObject& execution, const qint64 timeoutMs) {
    std::optional<protocol::JobSpec> spec = jobSpecOf(execution, timeoutMs);
    if (!spec)
        return QStringLiteral("A remote job takes argv or a shell command, cwd and env_delta.");
    if (timeoutMs < protocol::limits::minTimeoutMs || timeoutMs > protocol::limits::maxTimeoutMs)
        return QStringLiteral("A remote job's timeout must be from 1 second to 24 hours.");
    const auto worktree = sync::worktreeOf(spec->cwd);
    spec->cwd = worktree ? worktree->relative : QString();
    // The largest values the submit can give these fields, so the real frame fits as well.
    spec->epoch = protocol::limits::maxSafeInteger;
    spec->jobId = QString(protocol::limits::maxIdChars, u'j');
    spec->attempt = protocol::limits::maxAttempt;
    spec->repositoryKey = QString(32, u'k');
    spec->workspaceKey = QString(32, u'k');
    spec->objectFormat = QStringLiteral("sha256");
    spec->snapshotId = QString(64, u'0');
    spec->sourceHead = QString(64, u'0');
    const protocol::EncodeResult encoded =
        protocol::encode(protocol::Frame(*spec), protocol::Direction::DesktopToRunner);
    if (!encoded.ok()) {
        const QString field = encoded.error->field;
        if (field == QStringLiteral("env"))
            return QStringLiteral("A remote job's env_delta takes at most 64 names and 32 KiB, "
                                  "and no name starting with KETPLUS_.");
        if (field.startsWith(QStringLiteral("invocation")))
            return QStringLiteral("A remote job's command must be at most 256 arguments or a "
                                  "32 KiB shell command, with a nonempty program.");
        if (field == QStringLiteral("cwd"))
            return QStringLiteral("A remote job's cwd must be at most 1 KiB inside its "
                                  "worktree.");
        return QStringLiteral("The remote job spec is too large.");
    }
    return {};
}

} // namespace ketplus::daemon::runner::entry

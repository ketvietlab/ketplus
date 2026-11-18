#include "exec/ExecutionPlan.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>

namespace ketplus::runner::exec {
namespace {

const QString cwdMissing = QStringLiteral("cwd_missing");
const QString cwdOutside = QStringLiteral("cwd_outside_workspace");

// The real path of an existing directory, or empty.
QString realDirectory(const QString& path) {
    const QFileInfo info(path);
    return QDir::isAbsolutePath(path) && info.isDir() ? info.canonicalFilePath() : QString();
}

bool inside(const QString& path, const QString& root) {
    return path == root || path.startsWith(root == QStringLiteral("/") ? root : root + u'/');
}

bool validName(const QString& name) {
    return !name.isEmpty() && !name.contains(u'=') && !name.contains(QChar::Null);
}

PreparedLaunch refused(const QString& error) { return {{}, error}; }

} // namespace

PreparedLaunch prepareLaunch(const session::LaunchRequest& request,
                             const QMap<QString, QString>& runnerEnvironment) {
    QJsonObject execution;
    const protocol::Invocation& invocation = request.invocation;
    if (invocation.argv && !invocation.argv->isEmpty() && !invocation.shell) {
        QJsonArray argv;
        for (const QString& argument : *invocation.argv) {
            if (argument.contains(QChar::Null))
                return refused(QStringLiteral("invalid_invocation"));
            argv.append(argument);
        }
        if (argv.first().toString().isEmpty())
            return refused(QStringLiteral("invalid_invocation"));
        execution.insert(QStringLiteral("argv"), argv);
    } else if (invocation.shell && !invocation.shell->isEmpty() && !invocation.argv &&
               !invocation.shell->contains(QChar::Null)) {
        execution.insert(QStringLiteral("shell_command"), *invocation.shell);
    } else {
        return refused(QStringLiteral("invalid_invocation"));
    }

    const QString workspace = realDirectory(request.workspace);
    if (workspace.isEmpty())
        return refused(QStringLiteral("workspace_missing"));
    if (QDir::isAbsolutePath(request.cwd) || request.cwd.contains(QChar::Null))
        return refused(cwdOutside);
    const QString cwd = realDirectory(QDir(workspace).filePath(request.cwd));
    if (cwd.isEmpty())
        return refused(cwdMissing);
    if (!inside(cwd, workspace))
        return refused(cwdOutside);
    execution.insert(QStringLiteral("cwd"), cwd);

    QJsonObject delta;
    QJsonObject base;
    for (auto it = runnerEnvironment.cbegin(); it != runnerEnvironment.cend(); ++it) {
        base.insert(it.key(), it.value());
        if (!request.environment.contains(it.key()) && validName(it.key()))
            delta.insert(it.key(), QJsonValue::Null);
    }
    for (auto it = request.environment.cbegin(); it != request.environment.cend(); ++it) {
        if (!validName(it.key()) || it.value().contains(QChar::Null))
            return refused(QStringLiteral("invalid_environment"));
        delta.insert(it.key(), it.value());
    }
    execution.insert(QStringLiteral("env_delta"), delta);
    if (QJsonDocument(execution).toJson(QJsonDocument::Compact).size() +
            QJsonDocument(base).toJson(QJsonDocument::Compact).size() >
        maxLaunchBytes)
        return refused(QStringLiteral("launch_too_large"));
    return {execution, {}};
}

MountPlan planMounts(const QString& stateDirectory, const QString& workDirectory,
                     const QString& runtimeDirectory, const QString& workspace) {
    MountPlan plan;
    const QString state = realDirectory(stateDirectory);
    const QString work = realDirectory(workDirectory);
    const QString job = realDirectory(workspace);
    if (state.isEmpty() || work.isEmpty()) {
        plan.error = QStringLiteral("runner_directory_missing");
        return plan;
    }
    if (job.isEmpty() || job == work || !inside(job, work) || inside(job, state)) {
        plan.error = QStringLiteral("workspace_outside_work");
        return plan;
    }
    plan.hidden = {state, work};
    if (!runtimeDirectory.isEmpty()) {
        const QString runtime = realDirectory(runtimeDirectory);
        if (runtime.isEmpty() || inside(job, runtime)) {
            plan.error = QStringLiteral("runtime_directory_invalid");
            plan.hidden.clear();
            return plan;
        }
        plan.hidden.append(runtime);
    }
    plan.workspace = job;
    return plan;
}

QString executorTaskId(const QString& jobId, const qint64 attempt) {
    if (jobId.isEmpty() || attempt < 1)
        return {};
    const QByteArray digest =
        QCryptographicHash::hash(jobId.toUtf8(), QCryptographicHash::Sha256).toHex().left(32);
    return QString::fromLatin1(digest) + u'-' + QString::number(attempt);
}

session::CommandExit commandExit(const QJsonObject& result) {
    static const QRegularExpression code(QStringLiteral("^[a-z0-9_]{1,64}$"));
    session::CommandExit exit;
    const QString cleanup = result.value(QStringLiteral("cleanup_status")).toString();
    const QString state = result.value(QStringLiteral("state")).toString();
    const bool verified =
        state != QStringLiteral("needs_attention") &&
        (cleanup == QStringLiteral("verified") || cleanup == QStringLiteral("not_required"));
    exit.cleanupStatus = verified ? QStringLiteral("verified") : QStringLiteral("unverified");
    const auto fail = [&exit](const QString& reason) {
        exit.state = QStringLiteral("failed");
        exit.reason = reason;
        return exit;
    };
    if (!result.value(QStringLiteral("root_started")).toBool()) {
        const QString error = result.value(QStringLiteral("error")).toString();
        return fail(code.match(error).hasMatch() ? error : QStringLiteral("spawn_failed"));
    }
    if (state == QStringLiteral("timed_out"))
        return fail(QStringLiteral("timed_out"));
    if (state == QStringLiteral("interrupted"))
        return fail(QStringLiteral("executor_interrupted"));
    // The executor reports an exit code only: on Linux a signalled command exits 128 + n, and a
    // command that did not exit normally on macOS has none.
    const qint64 exitCode = result.value(QStringLiteral("exit_code")).toInteger(-1);
    if (exitCode < 0)
        return fail(QStringLiteral("exit_status_unknown"));
    exit.exitCode = exitCode;
    return exit;
}

} // namespace ketplus::runner::exec

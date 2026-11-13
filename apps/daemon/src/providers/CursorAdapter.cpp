#include "providers/CursorAdapter.h"
#include "providers/compat/ProviderVersionProbe.h"
#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>
#include <utility>

namespace ketplus::daemon {
namespace {

QString pinnedSpawnModel(const QString& model) {
    const QString trimmed = model.trimmed();
    if (trimmed.isEmpty() || trimmed == QStringLiteral("auto") ||
        trimmed == QStringLiteral("default") || trimmed == QStringLiteral("auto-smart"))
        return {};
    return trimmed;
}

bool isPlanLimitPrefix(const QString& text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return false;
    if (CursorAdapter::looksLikePlanLimit(trimmed))
        return true;
    return QStringLiteral("upgrade your plan to continue").startsWith(trimmed, Qt::CaseInsensitive);
}

CursorAdapter::ProcessInvocation wrapExecutable(const QString& executable,
                                                const QStringList& arguments) {
#if defined(Q_OS_WIN)
    const QString suffix = QFileInfo(executable).suffix().toLower();
    if (suffix == QStringLiteral("cmd") || suffix == QStringLiteral("bat")) {
        QStringList wrapped{QStringLiteral("/c"), QDir::toNativeSeparators(executable)};
        wrapped.append(arguments);
        return {QStringLiteral("cmd.exe"), wrapped};
    }
    if (suffix == QStringLiteral("ps1")) {
        QStringList wrapped{QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
                            QStringLiteral("Bypass"), QStringLiteral("-File"),
                            QDir::toNativeSeparators(executable)};
        wrapped.append(arguments);
        return {QStringLiteral("powershell.exe"), wrapped};
    }
#endif
    return {executable, arguments};
}

bool looksRunnable(const QFileInfo& info) {
    if (!info.isFile())
        return false;
#if defined(Q_OS_WIN)
    const QString suffix = info.suffix().toLower();
    if (suffix == QStringLiteral("exe") || suffix == QStringLiteral("cmd") ||
        suffix == QStringLiteral("bat") || suffix == QStringLiteral("ps1") || suffix.isEmpty())
        return true;
#endif
    return info.isExecutable();
}

} // namespace

CursorAdapter::CursorAdapter(const QString& executable, QObject* parent)
    : AcpAdapter(QStringLiteral("Cursor"), parent), executable_(executable, &findExecutable) {
    versionProbe_ = new compat::ProviderVersionProbe(
        compat::policy(QStringLiteral("cursor")),
        [](const QString& executable) {
            const ProcessInvocation invocation = versionInvocation(executable);
            return compat::ProviderVersionProbe::Invocation{invocation.program,
                                                            invocation.arguments};
        },
        this);
    catalogTimer_ = new QTimer(this);
    catalogTimer_->setSingleShot(true);
    connect(catalogTimer_, &QTimer::timeout, this, [this] {
        if (catalogProcess_ == nullptr)
            return;
        catalogProcess_->kill();
    });
}

CursorAdapter::~CursorAdapter() {
    // Owners destroy adapters at shutdown, after the SessionHost whose callbacks are still
    // pending; calling them here would use the freed host. Drop them instead of failing them.
    pending_.clear();
    queued_.clear();
    stop();
}

bool CursorAdapter::isAvailable() const { return !executable_.path().isEmpty(); }

QString CursorAdapter::missingMessage() const { return missingExecutableMessage(); }

QString CursorAdapter::executable() const { return executable_.path(); }

QString CursorAdapter::installCommand() {
#if defined(Q_OS_WIN)
    return QStringLiteral("irm 'https://cursor.com/install?win32=true' | iex");
#else
    return QStringLiteral("curl https://cursor.com/install -fsS | bash");
#endif
}

QString CursorAdapter::loginCommand() { return QStringLiteral("agent login"); }

QString CursorAdapter::missingExecutableMessage() {
    return QStringLiteral("Cursor CLI was not found. Install it in a terminal, then send again:\n\n"
                          "%1")
        .arg(installCommand());
}

QString CursorAdapter::notAuthenticatedMessage() {
#if defined(Q_OS_WIN)
    return QStringLiteral(
               "Cursor CLI is not signed in. In PowerShell, run:\n\n%1\n\nIf that command is not "
               "found:\n\n& \"$env:LOCALAPPDATA\\cursor-agent\\agent.cmd\" login\n\nThen send "
               "again. KetPlus does not store Cursor credentials.")
        .arg(loginCommand());
#else
    return QStringLiteral("Cursor CLI is not signed in. In a terminal, run:\n\n%1\n\nThen send "
                          "again. KetPlus does not store Cursor credentials.")
        .arg(loginCommand());
#endif
}

QString CursorAdapter::planLimitMessage() {
    const QString email = cliAuthEmail();
    const QString account = email.isEmpty()
                                ? QStringLiteral("the Cursor Agent CLI account")
                                : QStringLiteral("the Cursor Agent CLI account (%1)").arg(email);
    return QStringLiteral(
               "Cursor blocked this model for %1. Cursor desktop and Agent CLI use separate "
               "logins — if the desktop app is signed into a different email that has this model, "
               "sign the CLI into that account in a terminal:\n\n%2\n\nThen send again, or choose "
               "Auto / another model available on this CLI plan. "
               "https://cursor.com/dashboard/usage")
        .arg(account, loginCommand());
}

bool CursorAdapter::looksLikeUsageLimit(const QString& message) {
    static const QRegularExpression expression(
        QStringLiteral("reached (?:its|your) usage limit|usage limit (?:reached|exceeded)"),
        QRegularExpression::CaseInsensitiveOption);
    return expression.match(message).hasMatch();
}

qint64 CursorAdapter::usageLimitResetsAt(const QString& message) {
    static const QRegularExpression expression(
        QStringLiteral("return on (\\d{1,2})/(\\d{1,2})/(\\d{4})"),
        QRegularExpression::CaseInsensitiveOption);
    const QRegularExpressionMatch match = expression.match(message);
    if (!match.hasMatch())
        return 0;
    const QDate date(match.captured(3).toInt(), match.captured(1).toInt(),
                     match.captured(2).toInt());
    return date.isValid() ? date.startOfDay().toSecsSinceEpoch() : 0;
}

QString CursorAdapter::failureMessage(const QString& cursorMessage) {
    QString said = cursorMessage.trimmed();
    if (said.startsWith(QStringLiteral("ActionRequiredError:")))
        said = said.mid(20).trimmed();
    if (looksLikeAuthError(said))
        return notAuthenticatedMessage();
    // Checked before the plan wording: a used-up team can also be told to upgrade, and sending
    // it to log in again or pick Auto would not help.
    if (looksLikeUsageLimit(said))
        return said;
    if (looksLikePlanLimit(said))
        return QStringLiteral("%1\n\nCursor said: %2").arg(planLimitMessage(), said);
    return said;
}

bool CursorAdapter::looksLikePlanLimit(const QString& message) {
    const QString lower = message.trimmed().toLower();
    return lower.contains(QStringLiteral("upgrade your plan")) ||
           lower.contains(QStringLiteral("named models unavailable")) ||
           lower.contains(QStringLiteral("free plans can only use auto")) ||
           lower.contains(QStringLiteral("switch to auto or upgrade"));
}

QString CursorAdapter::cliAuthEmail() {
    const QString path = QDir::home().filePath(QStringLiteral(".cursor/cli-config.json"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    return root.value(QStringLiteral("authInfo"))
        .toObject()
        .value(QStringLiteral("email"))
        .toString()
        .trimmed();
}

QStringList CursorAdapter::wellKnownExecutableCandidates() {
    QStringList candidates;
#if defined(Q_OS_WIN)
    const QString localAppData = qEnvironmentVariable(
        "LOCALAPPDATA", QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation));
    const QDir cursorAgent(QDir(localAppData).filePath(QStringLiteral("cursor-agent")));
    for (const QString& name : {QStringLiteral("agent.cmd"), QStringLiteral("cursor-agent.cmd"),
                                QStringLiteral("agent.exe"), QStringLiteral("cursor-agent.exe")})
        candidates.append(cursorAgent.filePath(name));
#else
    candidates.append(QDir::home().filePath(QStringLiteral(".local/bin/agent")));
    candidates.append(QDir::home().filePath(QStringLiteral(".local/bin/cursor-agent")));
    candidates.append(QStringLiteral("/opt/homebrew/bin/agent"));
    candidates.append(QStringLiteral("/usr/local/bin/agent"));
#endif
    return candidates;
}

CursorAdapter::ProcessInvocation CursorAdapter::processInvocation(const QString& executable,
                                                                  const QString& model) {
    QStringList arguments;
    const QString pinned = pinnedSpawnModel(model);
    if (!pinned.isEmpty())
        arguments << QStringLiteral("--model") << pinned;
    arguments << QStringLiteral("acp");
    return wrapExecutable(executable, arguments);
}

CursorAdapter::ProcessInvocation CursorAdapter::listModelsInvocation(const QString& executable) {
    return wrapExecutable(executable, {QStringLiteral("models")});
}

CursorAdapter::ProcessInvocation CursorAdapter::versionInvocation(const QString& executable) {
    return wrapExecutable(executable, {QStringLiteral("--version")});
}

QJsonArray CursorAdapter::permissionProfiles() {
    return {QJsonObject{{QStringLiteral("id"), QStringLiteral("read-only")},
                        {QStringLiteral("label"), QStringLiteral("Read only")},
                        {QStringLiteral("description"),
                         QStringLiteral("Ask mode: inspect files without edits")},
                        {QStringLiteral("elevated"), false}},
            QJsonObject{{QStringLiteral("id"), QStringLiteral("workspace-write")},
                        {QStringLiteral("label"), QStringLiteral("Workspace")},
                        {QStringLiteral("description"),
                         QStringLiteral("Agent mode; Cursor asks before tools")},
                        {QStringLiteral("elevated"), false}},
            QJsonObject{
                {QStringLiteral("id"), QStringLiteral("danger-full-access")},
                {QStringLiteral("label"), QStringLiteral("Full access")},
                {QStringLiteral("description"), QStringLiteral("Skip Cursor permission prompts")},
                {QStringLiteral("elevated"), true}}};
}

QString CursorAdapter::permissionMode(const QString& profile) {
    if (profile == QStringLiteral("read-only"))
        return QStringLiteral("ask");
    return QStringLiteral("agent");
}

void CursorAdapter::runtimeStatus(std::function<void(const compat::Runtime&)> callback) {
    versionProbe_->check(executable_.path(),
                         [this, callback = std::move(callback)](const compat::Runtime& runtime) {
                             callback(narrowing_.apply(runtime));
                         });
}

bool CursorAdapter::supports(const QString& capability) const {
    if (narrowing_.removed(capability))
        return false;
    const auto runtime = versionProbe_->cached(executable_.path());
    return runtime ? runtime->supports(capability)
                   : versionProbe_->policy().baselineSupports(capability);
}

void CursorAdapter::call(const QString& method, const QJsonObject& params, Callback callback) {
    if (!isAvailable()) {
        callback({}, missingExecutableMessage());
        return;
    }
    if (method == QStringLiteral("model/list")) {
        listCliModels(std::move(callback));
        return;
    }
    if (method == QStringLiteral("turn/start")) {
        const auto launch = [this](QJsonObject params, Callback callback) {
            const QString cliModel =
                cliModelId(cachedCatalog_, params.value(QStringLiteral("model")).toString(),
                           params.value(QStringLiteral("effort")).toString());
            params.insert(QStringLiteral("model"), cliModel);
            // A process started with `--model` keeps that model whatever ACP is told later, so
            // Auto on it still ran the pinned model. Start it again unpinned while it is idle.
            if (ready_ && !spawnModel_.isEmpty() && pinnedSpawnModel(cliModel).isEmpty() &&
                turns_.isEmpty() && startingSessions_.isEmpty() && pending_.isEmpty() &&
                queued_.isEmpty())
                restartUnpinned();
            if (!ready_ && process_->state() == QProcess::NotRunning &&
                pendingSpawnModel_.isEmpty())
                pendingSpawnModel_ = pinnedSpawnModel(cliModel);
            AcpAdapter::call(QStringLiteral("turn/start"), params, std::move(callback));
        };
        if (cachedCatalog_.isEmpty()) {
            listCliModels([launch, params, callback = std::move(callback)](const QJsonObject&,
                                                                           const QString&) mutable {
                launch(params, std::move(callback));
            });
            return;
        }
        launch(params, std::move(callback));
        return;
    }
    AcpAdapter::call(method, params, std::move(callback));
}

void CursorAdapter::configurationCatalog(Callback callback) {
    listCliModels([callback = std::move(callback)](const QJsonObject& result,
                                                   const QString& providerError) {
        if (!providerError.isEmpty()) {
            callback({}, providerError);
            return;
        }
        QJsonArray catalog;
        for (const QJsonValue& value : result.value(QStringLiteral("data")).toArray()) {
            const QJsonObject source = value.toObject();
            catalog.append(
                QJsonObject{{QStringLiteral("id"), source.value(QStringLiteral("id"))},
                            {QStringLiteral("label"), source.value(QStringLiteral("label"))},
                            {QStringLiteral("efforts"), source.value(QStringLiteral("efforts"))},
                            {QStringLiteral("default_effort"),
                             source.value(QStringLiteral("default_effort"))}});
        }
        callback(QJsonObject{{QStringLiteral("catalog"), catalog}}, {});
    });
}

void CursorAdapter::restartUnpinned() {
    stopping_ = true;
    process_->terminate();
    if (!process_->waitForFinished(500)) {
        process_->kill();
        process_->waitForFinished(500);
    }
    // The finished handler settles the state and forgets the ACP sessions; the next turn loads
    // its session again. A process that would not die keeps running the turn as before.
    if (process_->state() == QProcess::NotRunning)
        pendingSpawnModel_.clear();
}

void CursorAdapter::configureProcess() {
    const ProcessInvocation invocation = processInvocation(executable_.path(), pendingSpawnModel_);
    spawnModel_ = pendingSpawnModel_;
    pendingSpawnModel_.clear();
    process_->setProgram(invocation.program);
    process_->setArguments(invocation.arguments);
}

bool CursorAdapter::looksLikeAuthError(const QString& message) {
    const QString lower = message.toLower();
    return lower.contains(QStringLiteral("not authenticated")) ||
           lower.contains(QStringLiteral("unauthenticated")) ||
           lower.contains(QStringLiteral("sign in")) ||
           lower.contains(QStringLiteral("cursor_login")) ||
           (lower.contains(QStringLiteral("login")) && lower.contains(QStringLiteral("auth")));
}

QString CursorAdapter::findExecutable() {
    const QString fromEnv = qEnvironmentVariable("KETPLUS_CURSOR_AGENT");
    if (!fromEnv.isEmpty()) {
        const QFileInfo info(fromEnv);
        if (looksRunnable(info))
            return info.absoluteFilePath();
    }
    for (const QString& name : {QStringLiteral("agent"), QStringLiteral("cursor-agent")}) {
        const QString fromPath = QStandardPaths::findExecutable(name);
        if (!fromPath.isEmpty())
            return fromPath;
    }
    for (const QString& candidate : wellKnownExecutableCandidates()) {
        const QFileInfo info(candidate);
        if (looksRunnable(info))
            return info.absoluteFilePath();
    }
    return {};
}

void CursorAdapter::checkLaunch(std::function<void(const QString&)> done) {
    versionProbe_->check(executable_.path(),
                         [done = std::move(done)](const compat::Runtime& runtime) {
                             done(runtime.blocksLaunch() ? runtime.message : QString{});
                         });
}

void CursorAdapter::resetProviderState() {
    questionOptionsByRequest_.clear();
    planRequests_.clear();
    spawnModel_.clear();
}

void CursorAdapter::stopAuxiliaryProcesses() {
    catalogTimer_->stop();
    finishCatalog(modelCatalog());
    if (catalogProcess_ && catalogProcess_->state() != QProcess::NotRunning) {
        catalogProcess_->kill();
        catalogProcess_->waitForFinished(500);
    }
}

QString CursorAdapter::sessionPermissionMode(const QString& profile) const {
    return permissionMode(profile);
}

QString CursorAdapter::sessionError(const QString& error) const {
    return looksLikeAuthError(error) ? notAuthenticatedMessage() : error;
}

QString CursorAdapter::turnFailureMessage(const QString& error) const {
    return failureMessage(error);
}

bool CursorAdapter::isFailureText(const QString& text) const {
    return looksLikePlanLimit(text) || looksLikeUsageLimit(text);
}

bool CursorAdapter::holdAssistantText(const QString& text) const { return isPlanLimitPrefix(text); }
qint64 CursorAdapter::failureResetsAt(const QString& error) const {
    return looksLikeUsageLimit(error) ? usageLimitResetsAt(error) : 0;
}
} // namespace ketplus::daemon

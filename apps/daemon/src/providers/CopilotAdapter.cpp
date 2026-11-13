#include "providers/CopilotAdapter.h"
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>

namespace ketplus::daemon {
namespace {
QString safeExplanation(QString text) {
    // Provider output is untrusted. Never journal credentials echoed by a CLI diagnostic.
    for (const auto* key : {"COPILOT_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"}) {
        const QString token = qEnvironmentVariable(key);
        if (!token.isEmpty())
            text.replace(token, QStringLiteral("[redacted]"));
    }
    text.replace(QRegularExpression(QStringLiteral("(?:gh[pousr]_|github_pat_)[A-Za-z0-9_]+")),
                 QStringLiteral("[redacted]"));
    text.replace(QRegularExpression(QStringLiteral("(?i)(authorization\\s*[:=]\\s*)[^\\r\\n]+")),
                 QStringLiteral("\\1[redacted]"));
    text.remove(QRegularExpression(QStringLiteral("[\\x00-\\x08\\x0b\\x0c\\x0e-\\x1f\\x7f]")));
    return text.left(2000).trimmed();
}
} // namespace

CopilotAdapter::CopilotAdapter(const QString& executable, QObject* parent)
    : AcpAdapter(QStringLiteral("GitHub Copilot"), parent), explicitPath_(executable) {
    refreshDiscovery();
}

CopilotAdapter::~CopilotAdapter() {
    pending_.clear();
    queued_.clear();
    stop();
}

void CopilotAdapter::refreshDiscovery() const {
    if (lastDiscovery_.isValid() && lastDiscovery_.elapsed() < 2000)
        return;
    discovery_ = copilot_cli::findExecutable(explicitPath_, copilot_cli::systemDiscoveryContext());
    lastDiscovery_.restart();
}

QString CopilotAdapter::executable() const {
    refreshDiscovery();
    return discovery_.executable;
}
bool CopilotAdapter::isAvailable() const { return !executable().isEmpty(); }
QString CopilotAdapter::missingMessage() const {
    refreshDiscovery();
    return discovery_.error.isEmpty() ? copilot_cli::missingExecutableMessage() : discovery_.error;
}

bool CopilotAdapter::supports(const QString& capability) const {
    if (narrowing_.removed(capability))
        return false;
    if (capability == compat::capability::turnInterrupt)
        return true;
    return ready_ && (capability == compat::capability::images ||
                      capability == compat::capability::threadResume);
}

void CopilotAdapter::runtimeStatus(std::function<void(const compat::Runtime&)> done) {
    compat::Runtime runtime;
    runtime.provider = name();
    runtime.name = QStringLiteral("GitHub Copilot");
    runtime.executable = executable();
    runtime.status =
        isAvailable() ? compat::SupportStatus::Unknown : compat::SupportStatus::Missing;
    runtime.message = isAvailable()
                          ? QStringLiteral("GitHub Copilot uses negotiated ACP capabilities.")
                          : missingMessage();
    for (const auto capability : {compat::capability::turnInterrupt,
                                  compat::capability::threadResume, compat::capability::images})
        if (supports(QString(capability)))
            runtime.capabilities.append(QString(capability));
    done(runtime);
}

void CopilotAdapter::configurationCatalog(Callback done) {
    done({{QStringLiteral("catalog"), QJsonArray{}}}, {});
}

void CopilotAdapter::call(const QString& method, const QJsonObject& params, Callback done) {
    if (method == QStringLiteral("model/list")) {
        done({{QStringLiteral("data"), QJsonArray{}}}, {});
        return;
    }
    if (authenticationFailed_) {
        stop();
        authenticationFailed_ = false;
    }
    AcpAdapter::call(method, params, std::move(done));
}

void CopilotAdapter::checkLaunch(std::function<void(const QString&)> done) {
    const auto context = copilot_cli::systemDiscoveryContext();
    const auto invocation =
        copilot_cli::processInvocation(executable(), context.platform, context.environment);
    done(invocation.error);
}

void CopilotAdapter::configureProcess() {
    const auto context = copilot_cli::systemDiscoveryContext();
    const auto invocation =
        copilot_cli::processInvocation(executable(), context.platform, context.environment);
    process_->setProgram(invocation.program);
    process_->setArguments(invocation.arguments);
#ifdef Q_OS_WIN
    process_->setNativeArguments(invocation.nativeArguments);
#endif
}

void CopilotAdapter::authenticationFailed() {
    authenticationFailed_ = true;
    const auto failedProcess = process_->processId();
    QTimer::singleShot(0, this, [this, failedProcess] {
        if (authenticationFailed_ && process_->processId() == failedProcess)
            stop();
    });
}

QString CopilotAdapter::initializationError(const QJsonObject& result) const {
    return result.value(QStringLiteral("protocolVersion")).toInt(-1) == 1
               ? QString{}
               : QStringLiteral("GitHub Copilot requires ACP protocol version 1. Update Copilot "
                                "CLI and retry.");
}

QString CopilotAdapter::responseError(const QJsonObject& error) {
    const QString guidance = copilot_cli::authenticationGuidance({error, {}, std::nullopt});
    if (!guidance.isEmpty()) {
        authenticationFailed();
        return guidance;
    }
    QString explanation = error.value(QStringLiteral("message")).toString();
    if (explanation.isEmpty())
        explanation = error.value(QStringLiteral("data"))
                          .toObject()
                          .value(QStringLiteral("message"))
                          .toString();
    explanation = safeExplanation(explanation);
    return explanation.isEmpty() ? QStringLiteral("GitHub Copilot request failed.") : explanation;
}

QString CopilotAdapter::processFailure(const QString& fallback, std::optional<int> exitCode) {
    if (authenticationFailed_)
        return copilot_cli::notAuthenticatedMessage();
    const auto guidance =
        copilot_cli::authenticationGuidance({{}, QString::fromUtf8(stderrBuffer_), exitCode});
    if (!guidance.isEmpty()) {
        authenticationFailed_ = true;
        return guidance;
    }
    return safeExplanation(fallback);
}

QString CopilotAdapter::sessionError(const QString& error) const {
    if (authenticationFailed_)
        return copilot_cli::notAuthenticatedMessage();
    const auto guidance = copilot_cli::authenticationGuidance(
        {{{QStringLiteral("message"), error}}, {}, std::nullopt});
    return guidance.isEmpty() ? safeExplanation(error) : guidance;
}
QString CopilotAdapter::turnFailureMessage(const QString& error) const {
    return sessionError(error);
}

int CopilotAdapter::requestTimeoutMs(const QString& method) const {
    return method == QStringLiteral("session/prompt") ? 30 * 60 * 1000 : 30 * 1000;
}
} // namespace ketplus::daemon

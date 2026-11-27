#include "runner/link/LinkLog.h"

Q_LOGGING_CATEGORY(lcRunnerLink, "ketplus.daemon.runner.link")

#include "protocol/FrameRules.h"

namespace ketplus::daemon::runner::link {

QString loggableCode(const QJsonValue& code) {
    if (code.isString() && ketplus::runner::protocol::rules::isToken(code.toString(), 64))
        return code.toString();
    return QStringLiteral("unknown");
}

} // namespace ketplus::daemon::runner::link

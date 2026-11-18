#include "runner/jobs/RemoteJobLabels.h"

#include <QRegularExpression>
#include <QSet>

namespace ketplus::daemon::runner::jobs {

bool isRunsOnLabel(const QString& label) {
    static const QRegularExpression pattern(
        QStringLiteral("^(os:linux|arch:(x86_64|arm64)|[a-z0-9][a-z0-9._-]{0,63})$"));
    return pattern.match(label).hasMatch();
}

bool isValidRunsOn(const QStringList& labels) {
    if (labels.isEmpty() || labels.size() > maximumRunsOn)
        return false;
    QSet<QString> seen;
    for (const QString& label : labels) {
        if (!isRunsOnLabel(label) || seen.contains(label))
            return false;
        seen.insert(label);
    }
    return true;
}

} // namespace ketplus::daemon::runner::jobs

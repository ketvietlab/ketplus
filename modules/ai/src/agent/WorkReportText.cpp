#include "agent/WorkReportText.h"

#include "agent/TranscriptSanitizer.h"

#include <QStringList>
#include <QVariantList>

namespace ketplus::agent_message {
namespace {

QString cell(QString text) {
    return TranscriptSanitizer::text(std::move(text))
        .replace(QLatin1Char('|'), QStringLiteral("\\|"))
        .replace(QLatin1Char('\n'), QLatin1Char(' '));
}

QString owner(const QString& name) {
    if (name == QStringLiteral("user"))
        return QStringLiteral("You");
    if (name == QStringLiteral("agent"))
        return QStringLiteral("Agent");
    return name.isEmpty() ? QString{} : QStringLiteral("@agent/") + name;
}

QString status(const QString& value) {
    if (value == QStringLiteral("active"))
        return QStringLiteral("In progress");
    return value.isEmpty() ? value : value.at(0).toUpper() + value.mid(1);
}

} // namespace

QString workReportText(const QVariantMap& report) {
    const QVariantList items = report.value(QStringLiteral("items")).toList();
    if (items.isEmpty())
        return {};
    int done = 0;
    int total = 0;
    QStringList rows{
        QStringLiteral("| Task | Status | Result or remaining work | Next and owner |"),
        QStringLiteral("| --- | --- | --- | --- |")};
    for (const QVariant& value : items) {
        const QVariantMap item = value.toMap();
        const QString state = item.value(QStringLiteral("status")).toString();
        if (state != QStringLiteral("cancelled")) {
            ++total;
            if (state == QStringLiteral("done"))
                ++done;
        }
        QString result = item.value(QStringLiteral("evidence")).toString();
        const QString remaining = item.value(QStringLiteral("remaining")).toString();
        if (!remaining.isEmpty())
            result += (result.isEmpty() ? QString{} : QStringLiteral("; ")) + remaining;
        const QVariantMap next = item.value(QStringLiteral("next")).toMap();
        const QString action = next.value(QStringLiteral("action")).toString();
        rows.append(QStringLiteral("| %1 | %2 | %3 | %4 |")
                        .arg(cell(item.value(QStringLiteral("title")).toString()), status(state),
                             cell(result),
                             action.isEmpty()
                                 ? QString{}
                                 : cell(action + QStringLiteral(" · ") +
                                        owner(next.value(QStringLiteral("owner")).toString()))));
    }
    QString title = report.value(QStringLiteral("title")).toString();
    if (title.isEmpty())
        title = QStringLiteral("Work report");
    return QStringLiteral("%1 · %2/%3 done\n").arg(cell(title)).arg(done).arg(total) +
           rows.join(QLatin1Char('\n'));
}

} // namespace ketplus::agent_message

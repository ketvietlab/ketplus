#include "agent/TranscriptTurnFiles.h"

#include <QHash>
#include <QString>
#include <algorithm>

namespace ketplus::agent_message {

QVariantList turnChangedFiles(const QList<QVariantMap>& rows, const qsizetype end) {
    if (end < 0 || end >= rows.size())
        return {};
    const QString turn = rows.at(end).value(QStringLiteral("turnId")).toString();
    // The turn's rows, oldest first: back to its prompt or to another turn's row.
    qsizetype first = end;
    while (first > 0) {
        const QVariantMap& previous = rows.at(first - 1);
        if (previous.value(QStringLiteral("kind")) == QStringLiteral("user"))
            break;
        const QString previousTurn = previous.value(QStringLiteral("turnId")).toString();
        if (!turn.isEmpty() && !previousTurn.isEmpty() && previousTurn != turn)
            break;
        --first;
    }
    QVariantList files;
    QHash<QString, qsizetype> byPath;
    const auto add = [&](const QVariantMap& change) {
        const QString path = change.value(QStringLiteral("path")).toString();
        if (path.isEmpty())
            return;
        const auto found = byPath.constFind(path);
        if (found == byPath.cend()) {
            byPath.insert(path, files.size());
            files.append(QVariantMap{
                {QStringLiteral("path"), path},
                {QStringLiteral("destination"), change.value(QStringLiteral("destination"))},
                {QStringLiteral("operation"), change.value(QStringLiteral("operation"))},
                {QStringLiteral("added"), change.value(QStringLiteral("added")).toInt()},
                {QStringLiteral("removed"), change.value(QStringLiteral("removed")).toInt()},
                {QStringLiteral("known"), change.value(QStringLiteral("known")).toBool()}});
            return;
        }
        QVariantMap file = files.at(*found).toMap();
        file.insert(QStringLiteral("added"), file.value(QStringLiteral("added")).toInt() +
                                                 change.value(QStringLiteral("added")).toInt());
        file.insert(QStringLiteral("removed"), file.value(QStringLiteral("removed")).toInt() +
                                                   change.value(QStringLiteral("removed")).toInt());
        file.insert(QStringLiteral("known"), file.value(QStringLiteral("known")).toBool() &&
                                                 change.value(QStringLiteral("known")).toBool());
        if (!change.value(QStringLiteral("destination")).toString().isEmpty())
            file.insert(QStringLiteral("destination"), change.value(QStringLiteral("destination")));
        files[*found] = file;
    };
    for (qsizetype row = first; row < end; ++row) {
        const QVariantMap& item = rows.at(row);
        const QString subtype = item.value(QStringLiteral("subtype")).toString();
        if (subtype == QStringLiteral("fileChange")) {
            if (item.value(QStringLiteral("status")) == QStringLiteral("failed"))
                continue;
            for (const QVariant& change : item.value(QStringLiteral("fileChanges")).toList())
                add(change.toMap());
        } else if (subtype == QStringLiteral("daemonGroup")) {
            for (const QVariant& change : item.value(QStringLiteral("groupFiles")).toList())
                add(change.toMap());
        }
    }
    return files;
}

} // namespace ketplus::agent_message

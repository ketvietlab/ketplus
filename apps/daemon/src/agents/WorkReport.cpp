#include "agents/WorkReport.h"

#include "cloud/CloudEventProjection.h"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QStringList>

#include <algorithm>

namespace ketplus::daemon::work_report {
namespace {

const QStringList& statuses() {
    static const QStringList values{QStringLiteral("done"), QStringLiteral("active"),
                                    QStringLiteral("waiting"), QStringLiteral("blocked"),
                                    QStringLiteral("cancelled")};
    return values;
}

// Checks stay separate: passing tests does not mean the build or the deployment passed.
const QStringList& checks() {
    static const QStringList values{QStringLiteral("code"),    QStringLiteral("tests"),
                                    QStringLiteral("build"),   QStringLiteral("package"),
                                    QStringLiteral("deploy"),  QStringLiteral("device"),
                                    QStringLiteral("capture"), QStringLiteral("review")};
    return values;
}

bool closed(const QJsonObject& item) {
    const QString status = item.value(QStringLiteral("status")).toString();
    return status == QStringLiteral("done") || status == QStringLiteral("cancelled");
}

// One line of redacted text: the same redactions as the transcript, then the field's cap.
QString clean(const QJsonValue& value, const qsizetype limit) {
    QString text = value.toString();
    text.replace(QLatin1Char('\n'), QLatin1Char(' ')).replace(QLatin1Char('\r'), QLatin1Char(' '));
    return capCloudText(sanitizeCloudText(text.simplified()), limit);
}

bool validKey(const QString& key) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9._-]{0,63}$"));
    return pattern.match(key).hasMatch();
}

// The agent, the user, or another conversation by its number.
bool validOwner(const QString& owner) {
    static const QRegularExpression conversation(QStringLiteral("^#?[1-9][0-9]{3,5}$"));
    return owner == QStringLiteral("agent") || owner == QStringLiteral("user") ||
           conversation.match(owner).hasMatch();
}

QString normalizedOwner(const QString& owner) {
    return owner.startsWith(QLatin1Char('#')) ? owner.mid(1) : owner;
}

// Applies the call's fields to an item. Returns an error that names the item.
QString apply(const QJsonObject& input, QJsonObject& item) {
    const QString key = item.value(QStringLiteral("key")).toString();
    const auto text = [&](const QString& field, const qsizetype limit) {
        if (input.contains(field))
            item.insert(field, clean(input.value(field), limit));
    };
    text(QStringLiteral("group"), maximumGroup);
    text(QStringLiteral("title"), maximumTitle);
    text(QStringLiteral("evidence"), maximumEvidence);
    text(QStringLiteral("remaining"), maximumRemaining);
    if (input.contains(QStringLiteral("status"))) {
        const QString status = input.value(QStringLiteral("status")).toString();
        if (!statuses().contains(status))
            return QStringLiteral("Item \"%1\" has an unknown status \"%2\"; use done, active, "
                                  "waiting, blocked or cancelled.")
                .arg(key, status);
        item.insert(QStringLiteral("status"), status);
    }
    if (input.contains(QStringLiteral("next"))) {
        const QJsonObject next = input.value(QStringLiteral("next")).toObject();
        const QString action = clean(next.value(QStringLiteral("action")), maximumAction);
        const QString owner = next.value(QStringLiteral("owner")).toString().trimmed();
        if (action.isEmpty()) {
            item.remove(QStringLiteral("next"));
        } else {
            if (!validOwner(owner))
                return QStringLiteral("Item \"%1\" has next.owner \"%2\"; use agent, user or a "
                                      "conversation number.")
                    .arg(key, owner);
            item.insert(QStringLiteral("next"),
                        QJsonObject{{QStringLiteral("action"), action},
                                    {QStringLiteral("owner"), normalizedOwner(owner)}});
        }
    }
    if (input.contains(QStringLiteral("verified"))) {
        QJsonArray verified;
        for (const QJsonValue& value : input.value(QStringLiteral("verified")).toArray()) {
            const QString check = value.toString();
            if (!checks().contains(check))
                return QStringLiteral("Item \"%1\" has an unknown check \"%2\"; use %3.")
                    .arg(key, check, checks().join(QStringLiteral(", ")));
            if (!verified.contains(check))
                verified.append(check);
        }
        item.insert(QStringLiteral("verified"), verified);
    }
    if (item.value(QStringLiteral("title")).toString().isEmpty())
        return QStringLiteral("Item \"%1\" needs a title.").arg(key);
    if (item.value(QStringLiteral("group")).toString().isEmpty())
        return QStringLiteral("Item \"%1\" needs a group.").arg(key);
    if (!item.contains(QStringLiteral("status")))
        return QStringLiteral("Item \"%1\" needs a status.").arg(key);
    if (item.value(QStringLiteral("status")).toString() == QStringLiteral("done") &&
        item.value(QStringLiteral("evidence")).toString().isEmpty() &&
        item.value(QStringLiteral("verified")).toArray().isEmpty())
        return QStringLiteral("Item \"%1\" is done without evidence; add the evidence or a "
                              "verified check, or report it as active.")
            .arg(key);
    return {};
}

QJsonObject projectedItem(QJsonObject item) {
    for (const auto& [field, limit] : {std::pair{QStringLiteral("group"), maximumGroup},
                                       std::pair{QStringLiteral("title"), maximumTitle},
                                       std::pair{QStringLiteral("evidence"), maximumEvidence},
                                       std::pair{QStringLiteral("remaining"), maximumRemaining}})
        if (item.contains(field))
            item.insert(field, clean(item.value(field), limit));
    if (item.contains(QStringLiteral("next"))) {
        QJsonObject next = item.value(QStringLiteral("next")).toObject();
        next.insert(QStringLiteral("action"),
                    clean(next.value(QStringLiteral("action")), maximumAction));
        item.insert(QStringLiteral("next"), next);
    }
    return item;
}

} // namespace

Merge merge(const QJsonObject& current, const QJsonObject& call, const qint64 turn) {
    Merge result;
    const QJsonArray inputs = call.value(QStringLiteral("items")).toArray();
    if (inputs.isEmpty()) {
        result.error = QStringLiteral("Report at least one item.");
        return result;
    }
    if (inputs.size() > maximumItemsPerCall) {
        result.error = QStringLiteral("Report at most %1 items per call.").arg(maximumItemsPerCall);
        return result;
    }
    QJsonArray items = current.value(QStringLiteral("items")).toArray();
    QHash<QString, qsizetype> byKey;
    for (qsizetype index = 0; index < items.size(); ++index)
        byKey.insert(items.at(index).toObject().value(QStringLiteral("key")).toString(), index);
    QSet<QString> seen;
    for (const QJsonValue& value : inputs) {
        const QJsonObject input = value.toObject();
        const QString key = input.value(QStringLiteral("key")).toString();
        if (!validKey(key)) {
            result.error = QStringLiteral("Item key \"%1\" is invalid; use 1-64 lowercase letters, "
                                          "digits, '.', '_' or '-', and keep it the same in "
                                          "later turns.")
                               .arg(key);
            return result;
        }
        if (seen.contains(key)) {
            result.error = QStringLiteral("Item \"%1\" appears twice in this call.").arg(key);
            return result;
        }
        seen.insert(key);
        const auto found = byKey.constFind(key);
        const bool existing = found != byKey.cend();
        QJsonObject item = existing ? items.at(*found).toObject()
                                    : QJsonObject{{QStringLiteral("key"), key},
                                                  {QStringLiteral("created_turn"), turn}};
        const QJsonObject before = item;
        if (const QString error = apply(input, item); !error.isEmpty()) {
            result.error = error;
            return result;
        }
        if (existing) {
            if (item != before) {
                item.insert(QStringLiteral("updated_turn"), turn);
                items[*found] = item;
                ++result.updated;
            }
        } else {
            item.insert(QStringLiteral("updated_turn"), turn);
            byKey.insert(key, items.size());
            items.append(item);
            ++result.added;
        }
    }
    // The oldest closed items leave first; open work never leaves silently.
    while (items.size() > maximumItems) {
        qsizetype oldest = -1;
        for (qsizetype index = 0; index < items.size(); ++index) {
            const QJsonObject item = items.at(index).toObject();
            if (closed(item) &&
                (oldest < 0 ||
                 item.value(QStringLiteral("updated_turn")).toInteger() <
                     items.at(oldest).toObject().value(QStringLiteral("updated_turn")).toInteger()))
                oldest = index;
        }
        if (oldest < 0) {
            result.error = QStringLiteral("The report holds %1 open items, the most it can; "
                                          "finish or cancel some first.")
                               .arg(maximumItems);
            return result;
        }
        items.removeAt(oldest);
    }
    result.ledger = current;
    if (call.contains(QStringLiteral("title")))
        result.ledger.insert(QStringLiteral("title"),
                             clean(call.value(QStringLiteral("title")), maximumTitle));
    result.ledger.insert(QStringLiteral("items"), items);
    result.ledger.insert(QStringLiteral("turn"), turn);
    result.ledger.insert(QStringLiteral("version"),
                         current.value(QStringLiteral("version")).toInteger() + 1);
    return result;
}

QJsonObject cloudProjection(const QJsonObject& ledger) {
    QJsonObject projected = ledger;
    projected.insert(QStringLiteral("title"),
                     clean(ledger.value(QStringLiteral("title")), maximumTitle));
    QJsonArray items;
    for (const QJsonValue& value : ledger.value(QStringLiteral("items")).toArray())
        items.append(projectedItem(value.toObject()));
    projected.insert(QStringLiteral("items"), items);
    return projected;
}

QJsonObject summary(const Merge& merge) {
    int done = 0;
    int counted = 0;
    int open = 0;
    for (const QJsonValue& value : merge.ledger.value(QStringLiteral("items")).toArray()) {
        const QString status = value.toObject().value(QStringLiteral("status")).toString();
        if (status == QStringLiteral("cancelled"))
            continue;
        ++counted;
        if (status == QStringLiteral("done"))
            ++done;
        else
            ++open;
    }
    return {{QStringLiteral("version"), merge.ledger.value(QStringLiteral("version"))},
            {QStringLiteral("added"), merge.added},
            {QStringLiteral("updated"), merge.updated},
            {QStringLiteral("done"), done},
            {QStringLiteral("total"), counted},
            {QStringLiteral("open"), open}};
}

} // namespace ketplus::daemon::work_report

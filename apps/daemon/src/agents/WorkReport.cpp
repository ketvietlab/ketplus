#include "agents/WorkReport.h"

#include "cloud/CloudEventProjection.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
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

// An item another one took over: a duplicate the app hides and nothing counts.
bool replaced(const QJsonObject& item) { return item.contains(QStringLiteral("replaced_by")); }

// The turn an item was closed in, kept while it stays closed: later calls may reword a finished
// item, and its age is when it finished. An item closed before this field existed has none.
void stampClosedTurn(const QJsonObject& before, QJsonObject& item, const qint64 turn) {
    if (!closed(item))
        item.remove(QStringLiteral("closed_turn"));
    else if (before.isEmpty() || !closed(before))
        item.insert(QStringLiteral("closed_turn"), turn);
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
        // A finished item has no next step left; a stale one would read as work still to do.
        if (closed(item)) {
            if (!input.contains(QStringLiteral("next")))
                item.remove(QStringLiteral("next"));
            if (!input.contains(QStringLiteral("remaining")))
                item.remove(QStringLiteral("remaining"));
        }
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
        stampClosedTurn(existing ? before : QJsonObject{}, item, turn);
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
    // Duplicates the call merges: the replacing item stands, the replaced ones are cancelled.
    for (const QJsonValue& value : inputs) {
        const QJsonObject input = value.toObject();
        if (!input.contains(QStringLiteral("replaces")))
            continue;
        const QString key = input.value(QStringLiteral("key")).toString();
        for (const QJsonValue& replaced : input.value(QStringLiteral("replaces")).toArray()) {
            const QString old = replaced.toString();
            const auto found = byKey.constFind(old);
            if (old == key || seen.contains(old) || found == byKey.cend()) {
                result.error =
                    old == key || seen.contains(old)
                        ? QStringLiteral("Item \"%1\" replaces \"%2\", which this call also "
                                         "reports.")
                              .arg(key, old)
                        : QStringLiteral("Item \"%1\" replaces \"%2\", which is not in the "
                                         "report.")
                              .arg(key, old);
                return result;
            }
            QJsonObject item = items.at(*found).toObject();
            const QJsonObject before = item;
            // Finished work keeps its status and evidence: a duplicate done item is only
            // marked, so the app hides it and nothing counts it twice.
            if (!closed(item)) {
                item.insert(QStringLiteral("status"), QStringLiteral("cancelled"));
                item.remove(QStringLiteral("next"));
                item.remove(QStringLiteral("remaining"));
                stampClosedTurn(before, item, turn);
            }
            item.insert(QStringLiteral("replaced_by"), key);
            if (item == before)
                continue;
            item.insert(QStringLiteral("updated_turn"), turn);
            items[*found] = item;
            ++result.updated;
        }
    }
    result.ledger = current;
    result.ledger.remove(QStringLiteral("items"));
    if (call.contains(QStringLiteral("title")))
        result.ledger.insert(QStringLiteral("title"),
                             clean(call.value(QStringLiteral("title")), maximumTitle));
    // The oldest closed items leave first; open work never leaves silently. An item counts its
    // encoding and the comma after it.
    const auto encodedSize = [](const QJsonValue& value) {
        return QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact).size() - 1;
    };
    qsizetype bytes = encodedSize(result.ledger) + 64; // turn, version and the items array
    for (const QJsonValue& value : items)
        bytes += encodedSize(value);
    while (items.size() > maximumItems || bytes > maximumLedgerBytes) {
        qsizetype oldest = -1;
        for (qsizetype index = 0; index < items.size(); ++index) {
            const QJsonObject item = items.at(index).toObject();
            // Duplicates leave before real history, then the oldest closed items.
            const auto rank = [](const QJsonObject& value) {
                return std::pair{!replaced(value),
                                 value.value(QStringLiteral("updated_turn")).toInteger()};
            };
            if (closed(item) && (oldest < 0 || rank(item) < rank(items.at(oldest).toObject())))
                oldest = index;
        }
        if (oldest < 0) {
            result.ledger = {};
            result.error =
                items.size() > maximumItems
                    ? QStringLiteral("The report holds %1 open items, the most it can; finish or "
                                     "cancel some first.")
                          .arg(maximumItems)
                    : QStringLiteral("The report is too large; shorten the evidence and remaining "
                                     "text, or finish or cancel some items first.");
            return result;
        }
        bytes -= encodedSize(items.at(oldest));
        items.removeAt(oldest);
    }
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
        if (status == QStringLiteral("cancelled") || replaced(value.toObject()))
            continue;
        ++counted;
        if (status == QStringLiteral("done"))
            ++done;
        else
            ++open;
    }
    const qint64 turn = merge.ledger.value(QStringLiteral("turn")).toInteger();
    QList<QJsonObject> openItems;
    QList<QJsonObject> doneItems;
    for (const QJsonValue& value : merge.ledger.value(QStringLiteral("items")).toArray()) {
        const QJsonObject item = value.toObject();
        if (!closed(item))
            openItems.append(item);
        else if (item.value(QStringLiteral("status")).toString() == QStringLiteral("done") &&
                 !replaced(item))
            doneItems.append(item);
    }
    const auto finishedTurn = [](const QJsonObject& item) {
        return item.contains(QStringLiteral("closed_turn"))
                   ? item.value(QStringLiteral("closed_turn")).toInteger()
                   : item.value(QStringLiteral("updated_turn")).toInteger();
    };
    std::stable_sort(doneItems.begin(), doneItems.end(),
                     [&](const QJsonObject& a, const QJsonObject& b) {
                         return finishedTurn(a) > finishedTurn(b);
                     });
    QJsonArray listedDone;
    for (const QJsonObject& item : std::as_const(doneItems)) {
        if (listedDone.size() >= maximumListedDoneItems)
            break;
        listedDone.append(QJsonObject{
            {QStringLiteral("key"), item.value(QStringLiteral("key"))},
            {QStringLiteral("title"),
             capCloudText(item.value(QStringLiteral("title")).toString(), maximumListedTitle)}});
    }
    std::stable_sort(openItems.begin(), openItems.end(),
                     [](const QJsonObject& a, const QJsonObject& b) {
                         return a.value(QStringLiteral("updated_turn")).toInteger() >
                                b.value(QStringLiteral("updated_turn")).toInteger();
                     });
    QJsonArray listed;
    int stale = 0;
    for (const QJsonObject& item : std::as_const(openItems)) {
        const qint64 updated = item.value(QStringLiteral("updated_turn")).toInteger();
        const bool old = turn - updated >= staleTurns;
        stale += old ? 1 : 0;
        if (listed.size() >= maximumListedOpenItems)
            continue;
        QJsonObject entry{
            {QStringLiteral("key"), item.value(QStringLiteral("key"))},
            {QStringLiteral("status"), item.value(QStringLiteral("status"))},
            {QStringLiteral("title"),
             capCloudText(item.value(QStringLiteral("title")).toString(), maximumListedTitle)},
            {QStringLiteral("updated_turn"), updated}};
        if (old)
            entry.insert(QStringLiteral("stale"), true);
        listed.append(entry);
    }
    QJsonObject answer{{QStringLiteral("version"), merge.ledger.value(QStringLiteral("version"))},
                       {QStringLiteral("added"), merge.added},
                       {QStringLiteral("updated"), merge.updated},
                       {QStringLiteral("done"), done},
                       {QStringLiteral("total"), counted},
                       {QStringLiteral("open"), open},
                       {QStringLiteral("stale"), stale},
                       {QStringLiteral("open_items"), listed},
                       {QStringLiteral("done_items"), listedDone}};
    QStringList reconcile;
    if (open > 0)
        reconcile.append(
            QStringLiteral("open_items are all the open items of this report, also ones from "
                           "earlier turns. Before ending the turn, report again by key each one "
                           "that is finished (done), no longer wanted (cancelled) or the same work "
                           "as another item (send that item with replaces). Confirm or close every "
                           "stale item."));
    if (!listedDone.isEmpty())
        reconcile.append(
            QStringLiteral("done_items are the most recently finished items. Do not report them "
                           "again unless their result changed. When two of them, or a done and an "
                           "open item, are the same work, send the one that stays with replaces; "
                           "the duplicate is hidden."));
    if (!reconcile.isEmpty()) {
        reconcile.append(QStringLiteral("Never add a new key for work listed here."));
        answer.insert(QStringLiteral("reconcile"), reconcile.join(QLatin1Char(' ')));
    }
    return answer;
}

} // namespace ketplus::daemon::work_report

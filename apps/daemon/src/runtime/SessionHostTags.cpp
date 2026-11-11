#include "runtime/SessionHost.h"

#include "storage/LocalJournal.h"

#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

namespace ketplus::daemon {
namespace {

constexpr qsizetype maximumTags = 8;
constexpr qsizetype maximumTagBytes = 64;
constexpr qsizetype maximumCatalogTags = 32;

QJsonObject error(const QString& code, const QString& message) {
    return {{QStringLiteral("ok"), false},
            {QStringLiteral("error_code"), code},
            {QStringLiteral("error_message"), message}};
}

bool printable(const QString& label) {
    return std::none_of(label.cbegin(), label.cend(), [](const QChar character) {
        return character.category() == QChar::Other_Control;
    });
}

bool validLabel(const QString& label) {
    return !label.isEmpty() && label.toUtf8().size() <= maximumTagBytes && printable(label);
}

QString findLabel(const QStringList& labels, const QString& label) {
    for (const QString& existing : labels)
        if (existing.compare(label, Qt::CaseInsensitive) == 0)
            return existing;
    return {};
}

} // namespace

QJsonObject SessionHost::tagCatalog() {
    return {{QStringLiteral("ok"), true}, {QStringLiteral("tags"), journal_->tagCatalog()}};
}

// The whole catalog at once, as Manage tags saves it. `renames` maps an old label to its new
// one; a conversation keeps a renamed label and loses one the catalog no longer lists.
QJsonObject SessionHost::setTagCatalog(const QJsonObject& request) {
    const QJsonObject payload = request.value(QStringLiteral("payload")).toObject();
    const QJsonValue requested = payload.value(QStringLiteral("tags"));
    const QJsonArray values = requested.toArray();
    if (!requested.isArray() || values.size() > maximumCatalogTags)
        return error(QStringLiteral("invalid_payload"),
                     QStringLiteral("`tags` must be an array of at most 32 tags."));
    static const QRegularExpression colorPattern(QStringLiteral("^#[0-9a-fA-F]{6}$"));
    QJsonArray catalog;
    QStringList labels;
    for (const auto& value : values) {
        const QJsonObject tag = value.toObject();
        const QString label = tag.value(QStringLiteral("label")).toString().trimmed();
        const QString color = tag.value(QStringLiteral("color")).toString();
        if (!validLabel(label) || !colorPattern.match(color).hasMatch())
            return error(QStringLiteral("invalid_payload"),
                         QStringLiteral("A tag needs printable text of 1 to 64 bytes and a "
                                        "#rrggbb colour."));
        if (!findLabel(labels, label).isEmpty())
            return error(QStringLiteral("invalid_payload"),
                         QStringLiteral("Each tag must have its own name."));
        labels.append(label);
        catalog.append(QJsonObject{{QStringLiteral("label"), label},
                                   {QStringLiteral("color"), color.toLower()}});
    }
    QHash<QString, QString> renames; // lower-case old label -> new label
    const QJsonObject requestedRenames = payload.value(QStringLiteral("renames")).toObject();
    for (auto it = requestedRenames.constBegin(); it != requestedRenames.constEnd(); ++it) {
        const QString target = findLabel(labels, it.value().toString().trimmed());
        if (target.isEmpty())
            return error(QStringLiteral("invalid_payload"),
                         QStringLiteral("A renamed tag must be in the new catalog."));
        renames.insert(it.key().trimmed().toLower(), target);
    }
    QStringList removed;
    for (const auto& value : journal_->tagCatalog()) {
        const QString label = value.toObject().value(QStringLiteral("label")).toString();
        if (!renames.contains(label.toLower()) && findLabel(labels, label).isEmpty())
            removed.append(label.toLower());
    }
    if (!journal_->setTagCatalog(catalog))
        return error(QStringLiteral("journal_error"), journal_->errorString());

    QJsonArray changedSessions;
    for (auto it = sessions_.begin(); it != sessions_.end(); ++it) {
        Session& session = it.value();
        QStringList tags;
        for (const QString& label : std::as_const(session.stored.tags)) {
            const QString key = label.toLower();
            if (removed.contains(key))
                continue;
            const QString next = renames.value(key, label);
            if (findLabel(tags, next).isEmpty())
                tags.append(next);
        }
        if (tags == session.stored.tags)
            continue;
        if (!journal_->setSessionTags(session.stored.id, session.stored.generation, tags))
            return error(QStringLiteral("journal_error"), journal_->errorString());
        session.stored.tags = tags;
        changedSessions.append(
            QJsonObject{{QStringLiteral("session_id"), session.stored.id},
                        {QStringLiteral("session_generation"), session.stored.generation},
                        {QStringLiteral("tags"), QJsonArray::fromStringList(tags)}});
    }
    emit noticeReady(
        QStringLiteral("tags.changed"),
        {{QStringLiteral("catalog"), catalog}, {QStringLiteral("sessions"), changedSessions}});
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("accepted"), true},
            {QStringLiteral("tags"), catalog},
            {QStringLiteral("sessions"), changedSessions}};
}

// Labels are the client's to name and colour; the daemon keeps which ones a conversation has.
QJsonObject SessionHost::setSessionTags(const QJsonObject& request) {
    Session* session = nullptr;
    if (const QJsonObject failure = checkedSession(request, &session); !failure.isEmpty())
        return failure;
    const QJsonValue requested =
        request.value(QStringLiteral("payload")).toObject().value(QStringLiteral("tags"));
    const QJsonArray values = requested.toArray();
    if (!requested.isArray() || values.size() > maximumTags)
        return error(QStringLiteral("invalid_payload"),
                     QStringLiteral("`tags` must be an array of at most 8 labels."));
    QStringList tags;
    for (const auto& value : values) {
        const QString label = value.toString().trimmed();
        if (!value.isString() || !validLabel(label))
            return error(QStringLiteral("invalid_payload"),
                         QStringLiteral("A tag must be printable text of 1 to 64 bytes."));
        // The same label in another case is one tag; the first spelling stays.
        if (!tags.contains(label, Qt::CaseInsensitive))
            tags.append(label);
    }
    if (tags != session->stored.tags) {
        if (!journal_->setSessionTags(session->stored.id, session->stored.generation, tags))
            return error(QStringLiteral("journal_error"), journal_->errorString());
        session->stored.tags = tags;
        emit noticeReady(QStringLiteral("tags.changed"),
                         {{QStringLiteral("sessions"),
                           QJsonArray{QJsonObject{
                               {QStringLiteral("session_id"), session->stored.id},
                               {QStringLiteral("session_generation"), session->stored.generation},
                               {QStringLiteral("tags"), QJsonArray::fromStringList(tags)}}}}});
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("accepted"), true},
            {QStringLiteral("session_id"), session->stored.id},
            {QStringLiteral("session_generation"), session->stored.generation},
            {QStringLiteral("tags"), QJsonArray::fromStringList(tags)}};
}

} // namespace ketplus::daemon

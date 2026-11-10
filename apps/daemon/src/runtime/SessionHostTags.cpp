#include "runtime/SessionHost.h"

#include "storage/LocalJournal.h"

#include <QJsonArray>

#include <algorithm>

namespace ketplus::daemon {
namespace {

constexpr qsizetype maximumTags = 8;
constexpr qsizetype maximumTagBytes = 64;

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

} // namespace

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
        if (!value.isString() || label.isEmpty() || label.toUtf8().size() > maximumTagBytes ||
            !printable(label))
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
    }
    return {{QStringLiteral("ok"), true},
            {QStringLiteral("accepted"), true},
            {QStringLiteral("session_id"), session->stored.id},
            {QStringLiteral("session_generation"), session->stored.generation},
            {QStringLiteral("tags"), QJsonArray::fromStringList(tags)}};
}

} // namespace ketplus::daemon

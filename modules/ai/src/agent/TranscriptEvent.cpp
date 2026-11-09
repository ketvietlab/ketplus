#include "agent/TranscriptEvent.h"

#include <QCryptographicHash>
#include <utility>

namespace ketplus {

namespace {

TranscriptCategory categoryFromName(const QString& name) {
    if (name == QStringLiteral("user"))
        return TranscriptCategory::User;
    if (name == QStringLiteral("final"))
        return TranscriptCategory::Final;
    if (name == QStringLiteral("question"))
        return TranscriptCategory::Question;
    if (name == QStringLiteral("error"))
        return TranscriptCategory::Error;
    if (name == QStringLiteral("system"))
        return TranscriptCategory::System;
    return TranscriptCategory::Activity;
}

TranscriptPhase phaseFromName(const QString& name) {
    if (name == QStringLiteral("started") || name == QStringLiteral("running") ||
        name == QStringLiteral("streaming") || name == QStringLiteral("inProgress"))
        return TranscriptPhase::Started;
    if (name == QStringLiteral("delta"))
        return TranscriptPhase::Delta;
    if (name == QStringLiteral("failed"))
        return TranscriptPhase::Failed;
    if (name == QStringLiteral("interrupted") || name == QStringLiteral("cancelled"))
        return TranscriptPhase::Interrupted;
    return TranscriptPhase::Completed;
}

} // namespace

QString TranscriptIdentity::stableKey() const {
    const QString effectiveItemId =
        itemId.isEmpty() ? QStringLiteral("sequence:%1").arg(sequence) : itemId;
    const QString material = QStringLiteral("%1\x1f%2\x1f%3\x1f%4\x1f%5")
                                 .arg(sessionId, QString::number(generation), turnId,
                                      effectiveItemId, QString::number(attempt));
    const QByteArray digest =
        QCryptographicHash::hash(material.toUtf8(), QCryptographicHash::Sha256).toHex().left(24);
    return QStringLiteral("transcript:%1").arg(QString::fromLatin1(digest));
}

QString transcriptCategoryName(const TranscriptCategory category) {
    switch (category) {
    case TranscriptCategory::User:
        return QStringLiteral("user");
    case TranscriptCategory::Final:
        return QStringLiteral("final");
    case TranscriptCategory::Question:
        return QStringLiteral("question");
    case TranscriptCategory::Error:
        return QStringLiteral("error");
    case TranscriptCategory::System:
        return QStringLiteral("system");
    case TranscriptCategory::Activity:
        return QStringLiteral("activity");
    }
    return QStringLiteral("activity");
}

QString transcriptPhaseName(const TranscriptPhase phase) {
    switch (phase) {
    case TranscriptPhase::Started:
        return QStringLiteral("started");
    case TranscriptPhase::Delta:
        return QStringLiteral("delta");
    case TranscriptPhase::Failed:
        return QStringLiteral("failed");
    case TranscriptPhase::Interrupted:
        return QStringLiteral("interrupted");
    case TranscriptPhase::Completed:
        return QStringLiteral("completed");
    }
    return QStringLiteral("completed");
}

bool TranscriptEvent::running() const {
    return phase == TranscriptPhase::Started || phase == TranscriptPhase::Delta;
}

QVariantMap TranscriptEvent::toVariantMap() const {
    return {{QStringLiteral("messageKey"), identity.stableKey()},
            {QStringLiteral("turnId"), identity.turnId},
            {QStringLiteral("itemId"), identity.itemId},
            {QStringLiteral("generation"), QVariant::fromValue(identity.generation)},
            {QStringLiteral("attempt"), identity.attempt},
            {QStringLiteral("sequence"), identity.sequence},
            {QStringLiteral("rawType"), rawType},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("body"), body},
            {QStringLiteral("meta"), meta},
            {QStringLiteral("timestamp"), timestamp},
            {QStringLiteral("filePath"), filePath},
            {QStringLiteral("subtype"), subtype},
            {QStringLiteral("command"), command},
            {QStringLiteral("running"), running()},
            {QStringLiteral("category"), transcriptCategoryName(category)},
            {QStringLiteral("eventType"), eventType},
            {QStringLiteral("phase"), transcriptPhaseName(phase)},
            {QStringLiteral("title"), title},
            {QStringLiteral("summary"), summary},
            {QStringLiteral("target"), target},
            {QStringLiteral("status"), transcriptPhaseName(phase)},
            {QStringLiteral("cwd"), cwd},
            {QStringLiteral("commandDescription"), commandDescription},
            {QStringLiteral("fileLocation"), fileLocation},
            {QStringLiteral("exitCode"), exitCode},
            {QStringLiteral("durationMs"), durationMs},
            {QStringLiteral("details"), details},
            {QStringLiteral("sanitizedPayload"), sanitizedPayload},
            {QStringLiteral("images"), images},
            {QStringLiteral("fileChanges"), fileChanges},
            {QStringLiteral("questions"), questions},
            {QStringLiteral("questionAnswers"), questionAnswers},
            {QStringLiteral("activityFacts"), activityFacts}};
}

TranscriptEvent TranscriptEvent::fromVariantMap(const QVariantMap& presentation,
                                                const TranscriptIdentity& identityValue,
                                                QString rawTypeValue) {
    TranscriptEvent event;
    event.identity = identityValue;
    event.category = categoryFromName(presentation.value(QStringLiteral("category")).toString());
    event.phase = phaseFromName(presentation.value(QStringLiteral("phase")).toString());
    event.rawType = std::move(rawTypeValue);
    event.eventType = presentation.value(QStringLiteral("eventType")).toString();
    event.subtype = presentation.value(QStringLiteral("subtype")).toString();
    event.kind = presentation.value(QStringLiteral("kind")).toString();
    event.body = presentation.value(QStringLiteral("body")).toString();
    event.meta = presentation.value(QStringLiteral("meta")).toString();
    event.timestamp = presentation.value(QStringLiteral("timestamp")).toString();
    event.title = presentation.value(QStringLiteral("title")).toString();
    event.summary = presentation.value(QStringLiteral("summary")).toString();
    event.target = presentation.value(QStringLiteral("target")).toString();
    event.filePath = presentation.value(QStringLiteral("filePath")).toString();
    event.command = presentation.value(QStringLiteral("command")).toString();
    event.cwd = presentation.value(QStringLiteral("cwd")).toString();
    event.commandDescription = presentation.value(QStringLiteral("commandDescription")).toString();
    event.fileLocation = presentation.value(QStringLiteral("fileLocation")).toString();
    event.exitCode = presentation.value(QStringLiteral("exitCode"));
    event.durationMs = presentation.value(QStringLiteral("durationMs"));
    event.details = presentation.value(QStringLiteral("details")).toMap();
    event.sanitizedPayload = presentation.value(QStringLiteral("sanitizedPayload")).toMap();
    event.images = presentation.value(QStringLiteral("images")).toList();
    event.fileChanges = presentation.value(QStringLiteral("fileChanges")).toList();
    event.questions = presentation.value(QStringLiteral("questions")).toList();
    event.questionAnswers = presentation.value(QStringLiteral("questionAnswers")).toMap();
    event.activityFacts = presentation.value(QStringLiteral("activityFacts")).toMap();
    return event;
}

} // namespace ketplus

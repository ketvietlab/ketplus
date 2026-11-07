#pragma once

#include <QJsonObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace ketplus {

enum class TranscriptCategory {
    User,
    Activity,
    Final,
    Question,
    Error,
    System,
};

enum class TranscriptPhase {
    Started,
    Delta,
    Completed,
    Failed,
    Interrupted,
};

struct TranscriptIdentity final {
    QString sessionId;
    quint64 generation{1};
    QString turnId;
    QString itemId;
    int attempt{1};
    qint64 sequence{0};

    [[nodiscard]] QString stableKey() const;
};

struct TranscriptEvent final {
    TranscriptIdentity identity;
    TranscriptCategory category{TranscriptCategory::Activity};
    TranscriptPhase phase{TranscriptPhase::Completed};
    QString rawType;
    QString eventType;
    QString subtype;
    QString kind;
    QString body;
    QString meta;
    QString timestamp;
    QString title;
    QString summary;
    QString target;
    QString filePath;
    QString command;
    QString cwd;
    // What the agent said a command is for, and the file a read row opens.
    QString commandDescription;
    QString fileLocation;
    QVariant exitCode;
    QVariant durationMs;
    QVariantMap details;
    QVariantMap sanitizedPayload;
    QVariantList images;
    QVariantList fileChanges;
    QVariantList questions;
    QVariantMap questionAnswers;
    bool visible{true};

    [[nodiscard]] bool running() const;
    [[nodiscard]] QVariantMap toVariantMap() const;

    static TranscriptEvent fromVariantMap(const QVariantMap& presentation,
                                          const TranscriptIdentity& identity, QString rawType = {});
};

[[nodiscard]] QString transcriptCategoryName(TranscriptCategory category);
[[nodiscard]] QString transcriptPhaseName(TranscriptPhase phase);

} // namespace ketplus

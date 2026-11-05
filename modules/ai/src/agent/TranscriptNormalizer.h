#pragma once

#include "agent/TranscriptEvent.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QList>
#include <QSet>

#include <optional>

namespace ketplus {

class TranscriptSanitizer final {
  public:
    [[nodiscard]] static bool isSensitiveKey(const QString& key);
    [[nodiscard]] static QString text(QString value);
    [[nodiscard]] static QString url(const QString& value);
    [[nodiscard]] static QJsonValue json(const QJsonValue& value, const QString& key = {});
};

class ActivityNormalizer final {
  public:
    enum class ProtocolEventPolicy {
        NormalizeItem,
        MergeDelta,
        Approval,
        Error,
        Metadata,
        Generic,
    };

    struct Context final {
        QString sessionId;
        quint64 generation{1};
        QString turnId;
        qint64 sequence{0};
        int attempt{1};
    };

    [[nodiscard]] static TranscriptEvent
    normalize(const QJsonObject& item, TranscriptPhase lifecycle, const Context& context);
    [[nodiscard]] static TranscriptEvent normalize(const QJsonObject& item,
                                                   TranscriptPhase lifecycle) {
        return normalize(item, lifecycle, Context{});
    }
    [[nodiscard]] static QList<TranscriptEvent> normalizeTurn(const QJsonObject& turn,
                                                              const Context& context);
    [[nodiscard]] static QList<TranscriptEvent> normalizeTurn(const QJsonObject& turn) {
        return normalizeTurn(turn, Context{});
    }
    [[nodiscard]] static QSet<QString> registeredRawTypes();
    [[nodiscard]] static QSet<QString> registeredProtocolEventTypes();
    [[nodiscard]] static ProtocolEventPolicy protocolPolicy(const QString& eventType);
};

class TranscriptLifecycleMerger final {
  public:
    void apply(TranscriptEvent event);
    [[nodiscard]] const QList<TranscriptEvent>& events() const { return events_; }
    [[nodiscard]] QVariantList variantEvents() const;

  private:
    QList<TranscriptEvent> events_;
    QHash<QString, qsizetype> indexByKey_;
};

} // namespace ketplus

#include "providers/CursorAdapter.h"
#include "providers/CursorModelVariant.h"
#include <QHash>
#include <utility>

namespace ketplus::daemon {
namespace {
using namespace cursor_model;
bool isCursorAutoModel(const QString& model) {
    const QString id = model.trimmed().toLower();
    return id.isEmpty() || id == QStringLiteral("auto") || id == QStringLiteral("default") ||
           id == QStringLiteral("auto-smart");
}

struct AcpSelectValue final {
    QString raw;
    QString base;
    QHash<QString, QString> params;
    bool parameterized{false};
};

AcpSelectValue parseAcpSelectValue(const QString& raw) {
    AcpSelectValue parsed;
    parsed.raw = raw;
    const int bracket = raw.indexOf(QLatin1Char('['));
    if (bracket < 0) {
        parsed.base = raw;
        return parsed;
    }
    parsed.parameterized = true;
    parsed.base = raw.left(bracket);
    QString inner = raw.mid(bracket + 1);
    if (inner.endsWith(QLatin1Char(']')))
        inner.chop(1);
    const QStringList parts = inner.split(QLatin1Char(','), Qt::SkipEmptyParts);
    for (const QString& part : parts) {
        const int eq = part.indexOf(QLatin1Char('='));
        if (eq <= 0)
            continue;
        parsed.params.insert(part.left(eq).trimmed(), part.mid(eq + 1).trimmed());
    }
    return parsed;
}

QString normalizedAcpBase(QString base) {
    if (base.startsWith(QStringLiteral("cursor-")))
        base = base.mid(7);
    return base;
}

QJsonArray modelChoices(const QJsonArray& configOptions) {
    for (const QJsonValue& entry : configOptions) {
        const QJsonObject option = entry.toObject();
        const QString id = option.value(QStringLiteral("id")).toString();
        const QString category = option.value(QStringLiteral("category")).toString();
        if (id != QStringLiteral("model") && category != QStringLiteral("model"))
            continue;
        const QJsonArray choices = option.value(QStringLiteral("options")).toArray();
        if (!choices.isEmpty())
            return choices;
    }
    return {};
}

bool configHasChoice(const QJsonArray& configOptions, const QString& configId,
                     const QString& value = {}) {
    for (const QJsonValue& entry : configOptions) {
        const QJsonObject option = entry.toObject();
        if (option.value(QStringLiteral("id")).toString() != configId)
            continue;
        if (value.isEmpty())
            return true;
        for (const QJsonValue& choice : option.value(QStringLiteral("options")).toArray()) {
            if (choice.toObject().value(QStringLiteral("value")).toString() == value)
                return true;
        }
        return false;
    }
    return false;
}

QList<QPair<QString, QString>> extraModelOptions(const QString& requested, const QString& chosen,
                                                 const QJsonArray& options) {
    if (isCursorAutoModel(requested))
        return {};
    const CursorVariant variant = parseCursorVariant(requested, {});
    const AcpSelectValue parsed = parseAcpSelectValue(chosen);
    QList<QPair<QString, QString>> rest;
    if (parsed.parameterized) {
        const QString advertisedFast = parsed.params.value(QStringLiteral("fast"));
        if (variant.fast && advertisedFast == QStringLiteral("false") &&
            configHasChoice(options, QStringLiteral("fast")))
            rest.append({QStringLiteral("fast"), QStringLiteral("true")});
        else if (!variant.fast && advertisedFast == QStringLiteral("true") &&
                 configHasChoice(options, QStringLiteral("fast")))
            rest.append({QStringLiteral("fast"), QStringLiteral("false")});
        if (!variant.effort.isEmpty() && variant.effort != QStringLiteral("fast") &&
            parsed.params.value(QStringLiteral("effort")) != variant.effort &&
            configHasChoice(options, QStringLiteral("effort"), variant.effort))
            rest.append({QStringLiteral("effort"), variant.effort});
        return rest;
    }
    if (configHasChoice(options, QStringLiteral("effort"), variant.effort) &&
        variant.effort != QStringLiteral("fast"))
        rest.append({QStringLiteral("effort"), variant.effort});
    if (configHasChoice(options, QStringLiteral("fast"))) {
        rest.append({QStringLiteral("fast"),
                     variant.fast ? QStringLiteral("true") : QStringLiteral("false")});
    }
    return rest;
}

} // namespace
QString CursorAdapter::acpModelId(const QString& model) {
    if (isCursorAutoModel(model))
        return QStringLiteral("default");
    const CursorVariant variant = parseCursorVariant(model.trimmed(), {});
    QString id = variant.family;
    if (id.startsWith(QStringLiteral("cursor-")))
        id = id.mid(7);
    return id.isEmpty() ? model.trimmed() : id;
}

QString CursorAdapter::acpModelConfigValue(const QString& model, const QJsonArray& configOptions) {
    if (isCursorAutoModel(model))
        return QStringLiteral("default");
    const QString want = acpModelId(model);
    const CursorVariant variant = parseCursorVariant(model.trimmed(), {});
    const QJsonArray choices = modelChoices(configOptions);
    if (choices.isEmpty())
        return want;
    int bestScore = -1;
    QString best;
    for (const QJsonValue& choice : choices) {
        const QString raw = choice.toObject().value(QStringLiteral("value")).toString();
        if (raw.isEmpty())
            continue;
        const AcpSelectValue parsed = parseAcpSelectValue(raw);
        if (normalizedAcpBase(parsed.base) != want)
            continue;
        int score = 1;
        const QString advertisedFast = parsed.params.value(QStringLiteral("fast"));
        const QString advertisedEffort = parsed.params.value(QStringLiteral("effort"));
        if (variant.fast) {
            if (advertisedFast == QStringLiteral("true"))
                score += 40;
            else if (advertisedFast == QStringLiteral("false"))
                score -= 20;
        } else if (advertisedFast == QStringLiteral("false")) {
            score += 40;
        } else if (advertisedFast == QStringLiteral("true")) {
            score -= 20;
        } else {
            score += 8;
        }
        if (!variant.effort.isEmpty() && variant.effort != QStringLiteral("fast")) {
            if (advertisedEffort == variant.effort)
                score += 30;
            else if (!advertisedEffort.isEmpty() && variant.effort != QStringLiteral("medium"))
                score -= 5;
        }
        if (parsed.parameterized)
            score += 3;
        if (score > bestScore) {
            bestScore = score;
            best = raw;
        }
    }
    return best.isEmpty() ? want : best;
}

void CursorAdapter::applySessionModel(const QString& acpSessionId, const QString& model,
                                      const QJsonObject& sessionResult,
                                      std::function<void()> done) {
    const QString requested = model;
    const QString chosen = acpModelConfigValue(
        requested, sessionResult.value(QStringLiteral("configOptions")).toArray());
    sendCall(QStringLiteral("session/set_config_option"),
             {{QStringLiteral("sessionId"), acpSessionId},
              {QStringLiteral("configId"), QStringLiteral("model")},
              {QStringLiteral("value"), chosen}},
             [this, acpSessionId, requested, chosen,
              done = std::move(done)](const QJsonObject& result, const QString&) {
                 const QJsonArray options = result.value(QStringLiteral("configOptions")).toArray();
                 const QString refined = acpModelConfigValue(requested, options);
                 const auto extras = [this, acpSessionId, requested,
                                      done](const QJsonArray& options, const QString& value) {
                     applyConfigOptions(acpSessionId, extraModelOptions(requested, value, options),
                                        done);
                 };
                 if (!refined.isEmpty() && refined != chosen) {
                     sendCall(QStringLiteral("session/set_config_option"),
                              {{QStringLiteral("sessionId"), acpSessionId},
                               {QStringLiteral("configId"), QStringLiteral("model")},
                               {QStringLiteral("value"), refined}},
                              [extras, refined](const QJsonObject& next, const QString&) {
                                  extras(next.value(QStringLiteral("configOptions")).toArray(),
                                         refined);
                              });
                     return;
                 }
                 extras(options, chosen);
             });
}
} // namespace ketplus::daemon

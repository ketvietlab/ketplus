#include "runner/RunnerRelaySchema.h"

#include "pairing/PairingIdentity.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>

#include <cmath>
#include <limits>

namespace ketplus::daemon::runner {
namespace {

const QSet<QString>& pairStates() {
    static const QSet<QString> states{QStringLiteral("ticketed"), QStringLiteral("claimed"),
                                      QStringLiteral("active"),   QStringLiteral("expired"),
                                      QStringLiteral("rejected"), QStringLiteral("revoked")};
    return states;
}

const QSet<QString>& runnerStates() {
    static const QSet<QString> states{QStringLiteral("offline"),  QStringLiteral("idle"),
                                      QStringLiteral("busy"),     QStringLiteral("draining"),
                                      QStringLiteral("disabled"), QStringLiteral("removed")};
    return states;
}

std::optional<QString> text(const QJsonObject& object, const char* key, qsizetype minimum,
                            qsizetype maximum) {
    const QJsonValue value = object.value(QLatin1StringView(key));
    if (!value.isString())
        return std::nullopt;
    const QString string = value.toString();
    if (string.size() < minimum || string.size() > maximum)
        return std::nullopt;
    return string;
}

std::optional<int> positiveInt(const QJsonObject& object, const char* key) {
    const QJsonValue value = object.value(QLatin1StringView(key));
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    if (number != std::floor(number) || number < 1 || number > std::numeric_limits<qint32>::max())
        return std::nullopt;
    return static_cast<int>(number);
}

std::optional<QByteArray> rawBytes(const QJsonObject& object, const char* key, qsizetype size) {
    const auto encoded = text(object, key, 1, 512);
    if (!encoded)
        return std::nullopt;
    const auto decoded = ketplus::runner::pairing::decodeBase64Url(*encoded);
    if (!decoded || decoded->size() != size)
        return std::nullopt;
    return decoded;
}

std::optional<QStringList> labels(const QJsonObject& object) {
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9._-]*$"));
    const QJsonValue value = object.value(QLatin1StringView("labels"));
    if (!value.isArray() || value.toArray().size() > 32)
        return std::nullopt;
    QStringList result;
    for (const QJsonValue& entry : value.toArray()) {
        const QString label = entry.toString();
        if (!entry.isString() || label.isEmpty() || label.size() > 64 ||
            !pattern.match(label).hasMatch() || result.contains(label))
            return std::nullopt;
        result.append(label);
    }
    return result;
}

struct Platform {
    QString name;
    QString os;
    QString arch;
    QStringList labels;
};

std::optional<Platform> platform(const QJsonObject& object) {
    const auto name = text(object, "name", 1, 64);
    const auto os = text(object, "os", 1, 16);
    const auto arch = text(object, "arch", 1, 16);
    const auto list = labels(object);
    if (!name || !os || *os != QStringLiteral("linux") || !arch ||
        (*arch != QStringLiteral("x86_64") && *arch != QStringLiteral("arm64")) || !list)
        return std::nullopt;
    return Platform{*name, *os, *arch, *list};
}

std::optional<RunnerClaim> parseClaim(const QJsonObject& object) {
    const auto runnerId = text(object, "runnerId", 1, 128);
    const auto publicKey = rawBytes(object, "publicKey", ketplus::runner::pairing::publicKeyBytes);
    const auto keyVersion = positiveInt(object, "keyVersion");
    const auto described = platform(object);
    const auto issuedAt = text(object, "issuedAt", 1, 64);
    const auto signature = rawBytes(object, "signature", 64);
    if (!runnerId || !isRunnerId(*runnerId) || !publicKey || !keyVersion || !described ||
        !issuedAt || !signature)
        return std::nullopt;
    return RunnerClaim{*runnerId,         *publicKey,    *keyVersion,
                       described->name,   described->os, described->arch,
                       described->labels, *issuedAt,     *signature};
}

std::optional<RunnerSummary> parseSummary(const QJsonValue& value) {
    if (!value.isObject())
        return std::nullopt;
    const QJsonObject object = value.toObject();
    const auto runnerId = text(object, "runnerId", 1, 128);
    const auto described = platform(object);
    const auto state = text(object, "state", 1, 16);
    const auto version = text(object, "runnerVersion", 1, 64);
    const QJsonValue seen = object.value(QLatin1StringView("lastSeenAt"));
    const QJsonValue paired = object.value(QLatin1StringView("pairedWithThisMachine"));
    if (!runnerId || !isRunnerId(*runnerId) || !described || !state ||
        !runnerStates().contains(*state) || !version || !paired.isBool() ||
        (!seen.isNull() && !seen.isString()))
        return std::nullopt;
    RunnerSummary summary{*runnerId,       described->name,   described->os,
                          described->arch, described->labels, *state,
                          *version,        std::nullopt,      paired.toBool()};
    if (seen.isString()) {
        summary.lastSeenAt = parseTime(seen.toString());
        if (!summary.lastSeenAt)
            return std::nullopt;
    }
    return summary;
}

} // namespace

bool isRunnerId(const QString& value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_-]{1,128}$"));
    return pattern.match(value).hasMatch();
}

std::optional<QDateTime> parseTime(const QString& value) {
    if (value.isEmpty() || value.size() > 64)
        return std::nullopt;
    const QDateTime parsed = QDateTime::fromString(value, Qt::ISODateWithMs);
    if (!parsed.isValid() || parsed.timeSpec() == Qt::LocalTime)
        return std::nullopt;
    return parsed.toUTC();
}

std::optional<RunnerTicket> parseTicket(const QJsonValue& value) {
    const QJsonObject object = value.toObject();
    const auto ticketId = text(object, "ticketId", 1, 128);
    const auto secret = text(object, "ticketSecret", 32, 256);
    const auto expires = text(object, "expiresAt", 1, 64);
    const auto expiresAt = expires ? parseTime(*expires) : std::nullopt;
    if (!value.isObject() || !ticketId || !isRunnerId(*ticketId) || !secret || !expiresAt)
        return std::nullopt;
    return RunnerTicket{*ticketId, *secret, *expiresAt};
}

std::optional<RunnerTicketStatus> parseTicketStatus(const QJsonValue& value) {
    const QJsonObject object = value.toObject();
    const auto ticketId = text(object, "ticketId", 1, 128);
    const auto state = text(object, "pairState", 1, 16);
    if (!value.isObject() || !ticketId || !isRunnerId(*ticketId) || !state ||
        !pairStates().contains(*state))
        return std::nullopt;
    RunnerTicketStatus status{*ticketId, *state, std::nullopt};
    if (*state == QStringLiteral("claimed")) {
        status.claim = parseClaim(object);
        if (!status.claim)
            return std::nullopt;
    }
    return status;
}

std::optional<RunnerPage> parseRunnerPage(const QJsonValue& value) {
    const QJsonObject object = value.toObject();
    const QJsonValue runners = object.value(QLatin1StringView("runners"));
    const QJsonValue cursor = object.value(QLatin1StringView("nextCursor"));
    if (!value.isObject() || !runners.isArray() || runners.toArray().size() > 100 ||
        !(cursor.isNull() ||
          (cursor.isString() && !cursor.toString().isEmpty() && cursor.toString().size() <= 256)))
        return std::nullopt;
    RunnerPage page;
    QSet<QString> seen;
    for (const QJsonValue& entry : runners.toArray()) {
        auto summary = parseSummary(entry);
        if (!summary || seen.contains(summary->runnerId))
            return std::nullopt;
        seen.insert(summary->runnerId);
        page.runners.append(std::move(*summary));
    }
    if (cursor.isString())
        page.nextCursor = cursor.toString();
    return page;
}

std::optional<QString> parseStateAnswer(const QJsonValue& value, const QString& runnerId) {
    const QJsonObject object = value.toObject();
    const auto answered = text(object, "runnerId", 1, 128);
    const auto state = text(object, "state", 1, 16);
    if (!value.isObject() || !answered || *answered != runnerId || !state ||
        !runnerStates().contains(*state))
        return std::nullopt;
    return state;
}

std::optional<QString> parsePairState(const QJsonValue& value) {
    const auto state = text(value.toObject(), "pairState", 1, 16);
    if (!value.isObject() || !state || !pairStates().contains(*state))
        return std::nullopt;
    return state;
}

} // namespace ketplus::daemon::runner

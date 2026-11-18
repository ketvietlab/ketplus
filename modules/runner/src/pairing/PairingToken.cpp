#include "pairing/PairingToken.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingIdentity.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <limits>

namespace ketplus::runner::pairing {
namespace {

constexpr QLatin1StringView prefix("kpr1.");

bool validFields(const PairingTokenFields& fields) {
    const auto fingerprint = decodeBase64Url(fields.desktopFingerprint);
    return !fields.ticketId.isEmpty() && !fields.ticketSecret.isEmpty() &&
           !fields.workspaceId.isEmpty() && !fields.machineId.isEmpty() &&
           fields.machineKeyVersion >= 1 && fingerprint && fingerprint->size() == 32 &&
           (!fields.runnerId || !fields.runnerId->isEmpty());
}

} // namespace

PairingResult<PairingToken> PairingToken::create(PairingTokenFields fields) {
    if (!validFields(fields))
        return PairingError::TokenFieldType;
    PairingToken token(std::move(fields));
    if (token.encode().size() > maxPairingTokenBytes)
        return PairingError::TokenTooLong;
    return token;
}

PairingResult<PairingToken> PairingToken::parse(const QByteArray& text) {
    const QByteArray token = text.trimmed();
    if (token.size() > maxPairingTokenBytes)
        return PairingError::TokenTooLong;
    if (!token.startsWith(prefix.data()))
        return PairingError::TokenPrefix;
    const auto json = decodeBase64Url(QString::fromLatin1(token.mid(prefix.size())));
    if (!json)
        return PairingError::TokenEncoding;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(*json, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        // QJsonDocument reads only objects and arrays; a lone string or number is still JSON.
        const QJsonDocument scalar = QJsonDocument::fromJson('[' + *json + ']', &parseError);
        return parseError.error == QJsonParseError::NoError && scalar.array().size() == 1
                   ? PairingError::TokenNotObject
                   : PairingError::TokenEncoding;
    }
    if (!document.isObject())
        return PairingError::TokenNotObject;
    const QJsonObject object = document.object();
    for (const char* key : {"t", "s", "w", "m", "kv", "fp", "r"}) {
        if (!object.contains(QLatin1StringView(key)))
            return PairingError::TokenMissingField;
    }
    for (const char* key : {"t", "s", "w", "m", "fp"}) {
        if (!object.value(QLatin1StringView(key)).isString())
            return PairingError::TokenFieldType;
    }
    const QJsonValue version = object.value(QLatin1StringView("kv"));
    const QJsonValue runner = object.value(QLatin1StringView("r"));
    if (!version.isDouble() || !(runner.isNull() || runner.isString()))
        return PairingError::TokenFieldType;
    const double number = version.toDouble();
    if (number != static_cast<double>(static_cast<qint64>(number)) || number < 1 ||
        number > std::numeric_limits<int>::max())
        return PairingError::TokenFieldType;

    PairingTokenFields fields;
    fields.ticketId = object.value(QLatin1StringView("t")).toString();
    fields.ticketSecret = object.value(QLatin1StringView("s")).toString();
    fields.workspaceId = object.value(QLatin1StringView("w")).toString();
    fields.machineId = object.value(QLatin1StringView("m")).toString();
    fields.machineKeyVersion = static_cast<int>(number);
    fields.desktopFingerprint = object.value(QLatin1StringView("fp")).toString();
    if (runner.isString())
        fields.runnerId = runner.toString();
    if (!validFields(fields))
        return PairingError::TokenFieldType;
    return PairingToken(std::move(fields));
}

QByteArray PairingToken::encode() const {
    const QJsonObject object{
        {QStringLiteral("t"), m_fields.ticketId},
        {QStringLiteral("s"), m_fields.ticketSecret},
        {QStringLiteral("w"), m_fields.workspaceId},
        {QStringLiteral("m"), m_fields.machineId},
        {QStringLiteral("kv"), m_fields.machineKeyVersion},
        {QStringLiteral("fp"), m_fields.desktopFingerprint},
        {QStringLiteral("r"),
         m_fields.runnerId ? QJsonValue(*m_fields.runnerId) : QJsonValue(QJsonValue::Null)},
    };
    return QByteArray(prefix.data(), prefix.size()) +
           control_crypto::base64Url(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

PairingToken::Kind PairingToken::kind() const {
    return m_fields.runnerId ? Kind::AddDesktop : Kind::NewRunner;
}

QDebug operator<<(QDebug debug, const PairingToken& token) {
    const QDebugStateSaver saver(debug);
    debug.nospace() << "PairingToken("
                    << (token.kind() == PairingToken::Kind::NewRunner ? "new runner"
                                                                      : "add desktop")
                    << ", <redacted>)";
    return debug;
}

} // namespace ketplus::runner::pairing

#include "pairing/PairingPins.h"

#include "cloud/CryptoPrimitives.h"
#include "pairing/PairingIdentity.h"
#include "pairing/PairingSignatures.h"

#include <QJsonValue>

#include <limits>
#include <optional>

namespace ketplus::runner::pairing {
namespace {

constexpr int recordVersion = 1;

QString keyText(const QByteArray& key) {
    return QString::fromLatin1(control_crypto::base64Url(key));
}

// Field readers: nullopt for an absent field or one of the wrong type.
std::optional<QString> text(const QJsonObject& object, const char* key) {
    const QJsonValue value = object.value(QLatin1StringView(key));
    if (!value.isString() || value.toString().isEmpty())
        return std::nullopt;
    return value.toString();
}

std::optional<int> positive(const QJsonObject& object, const char* key) {
    const QJsonValue value = object.value(QLatin1StringView(key));
    if (!value.isDouble())
        return std::nullopt;
    const double number = value.toDouble();
    if (number != static_cast<double>(static_cast<qint64>(number)) || number < 1 ||
        number > std::numeric_limits<int>::max())
        return std::nullopt;
    return static_cast<int>(number);
}

std::optional<QByteArray> key(const QJsonObject& object, const char* name) {
    const auto value = text(object, name);
    if (!value)
        return std::nullopt;
    const auto bytes = decodeBase64Url(*value);
    if (!bytes || bytes->size() != publicKeyBytes)
        return std::nullopt;
    return bytes;
}

std::optional<QDateTime> time(const QJsonObject& object, const char* key) {
    const auto value = text(object, key);
    if (!value || !value->endsWith(QLatin1Char('Z')))
        return std::nullopt;
    const QDateTime parsed = QDateTime::fromString(*value, Qt::ISODateWithMs);
    if (!parsed.isValid())
        return std::nullopt;
    return parsed.toUTC();
}

bool currentVersion(const QJsonObject& object) {
    return positive(object, "version") == recordVersion;
}

} // namespace

PairingResult<RunnerPin> RunnerPin::pending(const PairingToken& token,
                                            const ReportedDesktop& desktop,
                                            const QString& pairingDigest) {
    if (const auto error = checkDesktopKeys(token, desktop))
        return *error;
    if (pairingDigest.isEmpty())
        return PairingError::PinInvalid;
    const PairingTokenFields& fields = token.fields();
    RunnerPin pin;
    pin.machineId = fields.machineId;
    pin.workspaceId = fields.workspaceId;
    pin.machineKeyVersion = fields.machineKeyVersion;
    pin.signingPublicKey = desktop.signingPublicKey;
    pin.agreementPublicKey = desktop.agreementPublicKey;
    pin.desktopFingerprint = fields.desktopFingerprint;
    pin.state = State::Pending;
    pin.ticketId = fields.ticketId;
    pin.pairingDigest = pairingDigest;
    return pin;
}

RunnerPin RunnerPin::activated(const QDateTime& when) const {
    RunnerPin pin = *this;
    pin.state = State::Active;
    pin.ticketId.clear();
    pin.pairingDigest.clear();
    pin.pairedAt = when.toUTC();
    return pin;
}

QJsonObject RunnerPin::toJson() const {
    QJsonObject object{
        {QStringLiteral("version"), recordVersion},
        {QStringLiteral("machineId"), machineId},
        {QStringLiteral("workspaceId"), workspaceId},
        {QStringLiteral("machineKeyVersion"), machineKeyVersion},
        {QStringLiteral("signingPublicKey"), keyText(signingPublicKey)},
        {QStringLiteral("agreementPublicKey"), keyText(agreementPublicKey)},
        {QStringLiteral("desktopFingerprint"), desktopFingerprint},
    };
    if (state == State::Active) {
        object.insert(QStringLiteral("state"), QStringLiteral("active"));
        object.insert(QStringLiteral("pairedAt"),
                      pairedAt ? QJsonValue(issuedAtString(*pairedAt)) : QJsonValue());
    } else {
        object.insert(QStringLiteral("state"), QStringLiteral("pending"));
        object.insert(QStringLiteral("ticketId"), ticketId);
        object.insert(QStringLiteral("pairingDigest"), pairingDigest);
        object.insert(QStringLiteral("pairedAt"), QJsonValue(QJsonValue::Null));
    }
    return object;
}

PairingResult<RunnerPin> RunnerPin::fromJson(const QJsonObject& object) {
    const auto machine = text(object, "machineId");
    const auto workspace = text(object, "workspaceId");
    const auto version = positive(object, "machineKeyVersion");
    const auto signing = key(object, "signingPublicKey");
    const auto agreement = key(object, "agreementPublicKey");
    const auto fingerprint = text(object, "desktopFingerprint");
    const auto state = text(object, "state");
    if (!currentVersion(object) || !machine || !workspace || !version || !signing || !agreement ||
        !fingerprint || !state)
        return PairingError::PinInvalid;
    if (!constantTimeEquals(pairing::desktopFingerprint(*signing, *agreement).toLatin1(),
                            fingerprint->toLatin1()))
        return PairingError::PinInvalid;
    RunnerPin pin;
    pin.machineId = *machine;
    pin.workspaceId = *workspace;
    pin.machineKeyVersion = *version;
    pin.signingPublicKey = *signing;
    pin.agreementPublicKey = *agreement;
    pin.desktopFingerprint = *fingerprint;
    if (*state == QStringLiteral("pending")) {
        const auto ticket = text(object, "ticketId");
        const auto digest = text(object, "pairingDigest");
        const auto digestBytes = digest ? decodeBase64Url(*digest) : std::nullopt;
        if (!ticket || !digestBytes || digestBytes->size() != 32 ||
            !object.value(QLatin1StringView("pairedAt")).isNull())
            return PairingError::PinInvalid;
        pin.state = State::Pending;
        pin.ticketId = *ticket;
        pin.pairingDigest = *digest;
    } else if (*state == QStringLiteral("active")) {
        const auto paired = time(object, "pairedAt");
        if (!paired)
            return PairingError::PinInvalid;
        pin.state = State::Active;
        pin.pairedAt = *paired;
    } else {
        return PairingError::PinInvalid;
    }
    return pin;
}

std::optional<PairingError> verifyApproval(const RunnerPin& pin, const QByteArray& signature,
                                           std::optional<int> reportedMachineKeyVersion) {
    if (pin.state != RunnerPin::State::Pending || pin.ticketId.isEmpty() ||
        pin.pairingDigest.isEmpty())
        return PairingError::PinInvalid;
    if (pin.signingPublicKey.size() != publicKeyBytes)
        return PairingError::InvalidKey;
    if (reportedMachineKeyVersion && *reportedMachineKeyVersion != pin.machineKeyVersion)
        return PairingError::KeyVersionMismatch;
    const ApprovalPayload payload{pin.ticketId, pin.pairingDigest};
    if (!control_crypto::verifyEd25519(pin.signingPublicKey, payload.bytes(), signature))
        return PairingError::InvalidSignature;
    return std::nullopt;
}

QJsonObject DesktopRunnerPin::toJson() const {
    return {
        {QStringLiteral("version"), recordVersion},
        {QStringLiteral("runnerId"), runnerId},
        {QStringLiteral("runnerKeyVersion"), runnerKeyVersion},
        {QStringLiteral("publicKey"), keyText(publicKey)},
        {QStringLiteral("runnerFingerprint"), runnerFingerprint},
        {QStringLiteral("name"), name},
        {QStringLiteral("pairedAt"), issuedAtString(pairedAt)},
    };
}

PairingResult<DesktopRunnerPin> DesktopRunnerPin::fromJson(const QJsonObject& object) {
    const auto runner = text(object, "runnerId");
    const auto version = positive(object, "runnerKeyVersion");
    const auto publicKey = key(object, "publicKey");
    const auto fingerprint = text(object, "runnerFingerprint");
    const auto name = text(object, "name");
    const auto paired = time(object, "pairedAt");
    if (!currentVersion(object) || !runner || !version || !publicKey || !fingerprint || !name ||
        !paired)
        return PairingError::PinInvalid;
    if (!constantTimeEquals(pairing::runnerFingerprint(*publicKey).toLatin1(),
                            fingerprint->toLatin1()))
        return PairingError::PinInvalid;
    DesktopRunnerPin pin;
    pin.runnerId = *runner;
    pin.runnerKeyVersion = *version;
    pin.publicKey = *publicKey;
    pin.runnerFingerprint = *fingerprint;
    pin.name = *name;
    pin.pairedAt = *paired;
    return pin;
}

} // namespace ketplus::runner::pairing

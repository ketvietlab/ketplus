#include "pairing/PairingResult.h"

namespace ketplus::runner::pairing {

QString errorCode(PairingError error) {
    switch (error) {
    case PairingError::TokenTooLong:
        return QStringLiteral("token_too_long");
    case PairingError::TokenPrefix:
        return QStringLiteral("token_prefix");
    case PairingError::TokenEncoding:
        return QStringLiteral("token_encoding");
    case PairingError::TokenNotObject:
        return QStringLiteral("token_not_object");
    case PairingError::TokenMissingField:
        return QStringLiteral("token_missing_field");
    case PairingError::TokenFieldType:
        return QStringLiteral("token_field_type");
    case PairingError::DesktopKeyMismatch:
        return QStringLiteral("desktop_key_mismatch");
    case PairingError::InvalidSignature:
        return QStringLiteral("invalid_signature");
    case PairingError::KeyVersionMismatch:
        return QStringLiteral("key_version_mismatch");
    case PairingError::IssuedAtInvalid:
        return QStringLiteral("issued_at_invalid");
    case PairingError::IssuedAtOutOfWindow:
        return QStringLiteral("issued_at_out_of_window");
    case PairingError::InvalidKey:
        return QStringLiteral("invalid_key");
    case PairingError::PinInvalid:
        return QStringLiteral("pin_invalid");
    }
    return QStringLiteral("unknown");
}

} // namespace ketplus::runner::pairing

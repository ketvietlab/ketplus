#pragma once

#include <QString>

#include <optional>
#include <utility>

namespace ketplus::runner::pairing {

// Errors carry no input: a token or key never reaches a message.
enum class PairingError {
    TokenTooLong,
    TokenPrefix,       // not a kpr1 token
    TokenEncoding,     // not base64url, or not JSON
    TokenNotObject,    // the JSON is not an object
    TokenMissingField, // a required field is absent
    TokenFieldType,    // a field has the wrong type or an invalid value
    DesktopKeyMismatch,
    InvalidSignature,
    KeyVersionMismatch,
    IssuedAtInvalid,     // not an RFC 3339 UTC time
    IssuedAtOutOfWindow, // more than five minutes from the caller's clock
    InvalidKey,          // a key of the wrong length
    PinInvalid,          // a stored pin record that does not read back
};

// A stable snake_case code, as on the wire where the protocol names one.
[[nodiscard]] QString errorCode(PairingError error);

template <typename T> class PairingResult {
  public:
    PairingResult(T value) : m_value(std::move(value)) {}
    PairingResult(PairingError error) : m_error(error) {}

    [[nodiscard]] bool ok() const { return m_value.has_value(); }
    explicit operator bool() const { return ok(); }
    [[nodiscard]] const T& value() const { return *m_value; }
    [[nodiscard]] PairingError error() const { return m_error; }

  private:
    std::optional<T> m_value;
    PairingError m_error = PairingError::InvalidKey;
};

} // namespace ketplus::runner::pairing

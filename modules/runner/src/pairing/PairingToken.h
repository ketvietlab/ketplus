#pragma once

#include "pairing/PairingResult.h"

#include <QByteArray>
#include <QString>

#include <optional>

class QDebug;

// The pairing token a desktop creates and the user copies to a runner node:
// `kpr1.` followed by base64url (no padding) of a JSON object
// {"t": ticketId, "s": ticketSecret, "w": workspaceId, "m": machineId,
//  "kv": machineKeyVersion, "fp": desktopFingerprint, "r": runnerId or null}.
// The token is a secret: it never appears in QDebug output or in an error.
namespace ketplus::runner::pairing {

// Longest token text accepted, after surrounding whitespace is removed.
inline constexpr qsizetype maxPairingTokenBytes = 2048;

struct PairingTokenFields {
    QString ticketId;
    QString ticketSecret;
    QString workspaceId;
    QString machineId;
    int machineKeyVersion = 0;
    QString desktopFingerprint;
    // Unset for registering a new runner; the runner's id for adding this desktop to it.
    std::optional<QString> runnerId;
};

class PairingToken {
  public:
    enum class Kind {
        NewRunner,  // `ketplus-runner register`
        AddDesktop, // `ketplus-runner pair`
    };

    // Checks every field: non-empty ids, a key version of at least 1 and a fingerprint that
    // decodes to 32 bytes. Fails with TokenFieldType otherwise.
    [[nodiscard]] static PairingResult<PairingToken> create(PairingTokenFields fields);
    // Surrounding ASCII whitespace (a trailing newline from a file or stdin) is ignored.
    // Fields the format does not name are ignored.
    [[nodiscard]] static PairingResult<PairingToken> parse(const QByteArray& text);

    [[nodiscard]] QByteArray encode() const;
    [[nodiscard]] Kind kind() const;
    [[nodiscard]] const PairingTokenFields& fields() const { return m_fields; }

  private:
    explicit PairingToken(PairingTokenFields fields) : m_fields(std::move(fields)) {}
    PairingTokenFields m_fields;
};

// Prints the kind only, never a field.
QDebug operator<<(QDebug debug, const PairingToken& token);

} // namespace ketplus::runner::pairing

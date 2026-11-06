#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace ketplus::daemon {

class CredentialStore;

// The remote devices this Mac trusts, recorded on this Mac (#87). The relay's device list is a
// hint: a device receives session keys, and its commands and peer offers are accepted, only while
// its id, key version and both public keys match a pin. A pin is written when the user approves
// a pairing here, after the claim digest is recomputed from the claimed keys. Devices that were
// active when pins were introduced are adopted once from the first device list (trust on first
// use), so existing pairings keep working.
class DevicePins final {
  public:
    explicit DevicePins(CredentialStore* store);

    // base64url(SHA-256(["<ticket>","<device>",<version>,"<agreement>","<signing>"])), the
    // relay's and the phone's formula; its first characters are the code compared on both screens.
    [[nodiscard]] static QString claimDigest(const QString& ticketId, const QJsonObject& device);
    // The six characters both screens show for a claim digest, derived as the phone does; empty
    // for a digest too short to give them (#89).
    [[nodiscard]] static QString confirmationCode(const QString& claimDigest);
    // base64url(SHA-256(signing public key || agreement public key)): the host identity the
    // pairing QR code carries, so the phone can pin this Mac (#89). Empty for invalid keys.
    [[nodiscard]] static QString hostFingerprint(const QJsonObject& keys);

    // Whether pins have been initialized; before that the first device list is adopted.
    [[nodiscard]] bool initialized(QString* error = nullptr) const;
    // Pins every active row of the relay's device list, once.
    bool adopt(const QJsonArray& devices, QString* error = nullptr) const;
    // Initializes an empty pin set, replacing any earlier one: a new enrollment trusts no device.
    bool reset(QString* error = nullptr) const;
    bool pin(const QJsonObject& device, QString* error = nullptr) const;
    // Removes every key version of the device.
    bool unpin(const QString& deviceId, QString* error = nullptr) const;
    // A device row (`id`, `keyVersion`, `keys`) that matches its pin.
    [[nodiscard]] bool trusts(const QJsonObject& device) const;
    // Keys read for a sender id and version (`/v2/device-keys`), matched against its pin.
    [[nodiscard]] bool trustsKeys(const QString& deviceId, int keyVersion,
                                  const QJsonObject& keys) const;
    // The active rows of a device list whose keys match their pins.
    [[nodiscard]] QJsonArray trusted(const QJsonArray& devices) const;

  private:
    [[nodiscard]] QJsonObject read(QString* error) const;
    bool write(const QJsonObject& pins, QString* error) const;
    CredentialStore* store_{nullptr};
};

} // namespace ketplus::daemon

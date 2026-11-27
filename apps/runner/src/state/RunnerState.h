#pragma once

#include "pairing/PairingPins.h"
#include "state/StateDirectory.h"

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <optional>

// The runner's files in its state directory:
//   runner.key       the 32-byte Ed25519 seed, raw
//   credential.json  {"version": 1, runnerId, workspaceId, keyVersion, refreshCredential}
//   config.json      {"version": 1, name, labels}
//   pins/<machineId>.json  one pairing::RunnerPin per desktop
// No file holds a pairing token, a ticket secret or an access token.
namespace ketplus::runner::app {

struct RunnerCredential {
    QString runnerId;
    QString workspaceId;
    int keyVersion = 1;
    QString refreshCredential;
};

struct RunnerConfig {
    QString name;
    QStringList labels;
};

struct RunnerIdentity {
    QByteArray privateKey; // 32-byte seed
    QByteArray publicKey;
    RunnerCredential credential;
    RunnerConfig config;
};

class RunnerState {
  public:
    explicit RunnerState(const StateDirectory& directory) : m_directory(directory) {}

    [[nodiscard]] const StateDirectory& directory() const { return m_directory; }

    // A key, credential or configuration is present, whole or partial.
    [[nodiscard]] bool hasIdentity() const;
    // All three files, checked. Without an identity the error says to register.
    [[nodiscard]] std::optional<RunnerIdentity> loadIdentity(QString* error) const;

    [[nodiscard]] bool saveKey(const QByteArray& privateKey, QString* error) const;
    [[nodiscard]] bool saveConfig(const RunnerConfig& config, QString* error) const;
    [[nodiscard]] bool saveCredential(const RunnerCredential& credential, QString* error) const;
    // Key, credential, configuration and every pin. The lock file stays.
    [[nodiscard]] bool removeIdentity(QString* error) const;

    [[nodiscard]] std::optional<QList<pairing::RunnerPin>> loadPins(QString* error) const;
    [[nodiscard]] bool savePin(const pairing::RunnerPin& pin, QString* error) const;
    [[nodiscard]] bool removePin(const QString& machineId, QString* error) const;

  private:
    const StateDirectory& m_directory;
};

} // namespace ketplus::runner::app

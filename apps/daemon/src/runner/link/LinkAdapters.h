#pragma once

#include "runner/link/LinkPorts.h"

#include <QHash>
#include <QPointer>

#include <functional>

class QTimer;

namespace ketplus::daemon::transport {
class WebRtcPeerLink;
}

// The real ports of the desktop's runner peer links.
namespace ketplus::daemon::runner::link {

// A WebRtcPeerLink in the answering role.
class WebRtcAnswerTransport final : public LinkTransport {
  public:
    explicit WebRtcAnswerTransport(LinkTransportEvents& events);
    ~WebRtcAnswerTransport() override;
    bool answer(const QString& offerSdp, const QJsonArray& iceServers) override;
    bool candidate(const QString& candidate, const QString& mid) override;
    bool send(TransportChannel channel, const QByteArray& bytes, bool binary) override;
    [[nodiscard]] quint64 buffered(TransportChannel channel) const override;
    void close() override;

  private:
    void release();
    QPointer<transport::WebRtcPeerLink> m_link;
};

class WebRtcAnswerTransportFactory final : public LinkTransportFactory {
  public:
    [[nodiscard]] std::unique_ptr<LinkTransport> create(LinkTransportEvents& events) override;
};

// Deadlines on QTimer, answered through `fired` on the thread that armed them.
class QtLinkClock final : public LinkClock {
  public:
    using Fired = std::function<void(const QString& connectionId)>;
    QtLinkClock() = default;
    ~QtLinkClock() override;
    void setFired(Fired fired) { m_fired = std::move(fired); }
    [[nodiscard]] QDateTime now() const override;
    void arm(const QString& connectionId, qint64 delayMs) override;
    void disarm(const QString& connectionId) override;

  private:
    Fired m_fired;
    QHash<QString, QTimer*> m_timers;
};

// The runner pins of the daemon's credential store, through a lookup the caller provides.
class FunctionRunnerPins final : public RunnerPins {
  public:
    using Lookup = std::function<std::optional<ketplus::runner::pairing::DesktopRunnerPin>(
        const QString& runnerId)>;
    explicit FunctionRunnerPins(Lookup lookup) : m_lookup(std::move(lookup)) {}
    [[nodiscard]] std::optional<ketplus::runner::pairing::DesktopRunnerPin>
    pin(const QString& runnerId) const override {
        return m_lookup ? m_lookup(runnerId) : std::nullopt;
    }

  private:
    Lookup m_lookup;
};

// This machine's identity and the machine key's signing function.
class FunctionMachineIdentity final : public MachineIdentity {
  public:
    using Sign = std::function<QByteArray(const QByteArray& bytes)>;
    FunctionMachineIdentity(QString workspaceId, QString machineId, int keyVersion, Sign sign)
        : m_workspaceId(std::move(workspaceId)), m_machineId(std::move(machineId)),
          m_keyVersion(keyVersion), m_sign(std::move(sign)) {}
    [[nodiscard]] QString workspaceId() const override { return m_workspaceId; }
    [[nodiscard]] QString machineId() const override { return m_machineId; }
    [[nodiscard]] int machineKeyVersion() const override { return m_keyVersion; }
    [[nodiscard]] QByteArray sign(const QByteArray& bytes) const override {
        return m_sign ? m_sign(bytes) : QByteArray();
    }

  private:
    QString m_workspaceId;
    QString m_machineId;
    int m_keyVersion = 0;
    Sign m_sign;
};

} // namespace ketplus::daemon::runner::link

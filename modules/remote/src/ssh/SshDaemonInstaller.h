#pragma once
#include "ssh/DaemonRelease.h"
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <functional>
#include <memory>

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;
class QTemporaryDir;
class QFile;
namespace ketplus::remote::ssh {
class SshTunnel;

// One explicit operation bound to a verified host identity. The UI asks to review metadata,
// then separately confirms the complete signed artifact and remote changes. ADR 0124.
class SshDaemonInstaller final : public QObject {
    Q_OBJECT
  public:
    enum class State {
        Idle,
        Preflight,
        Manifest,
        Consent,
        Download,
        Installing,
        Connecting,
        RollingBack,
        Succeeded,
        Failed,
        Cancelled
    };
    Q_ENUM(State)
    explicit SshDaemonInstaller(SshTunnel* tunnel,
                                DaemonReleaseChannel channel = DaemonReleaseChannel::production(),
                                QObject* parent = nullptr);
    ~SshDaemonInstaller() override;
    [[nodiscard]] State state() const { return state_; }
    // "install" selects install/upgrade after preflight; "start" and "uninstall" are managed
    // receipts only and do not download anything. Calling review authorizes metadata fetch,
    // not remote writes. No automatic reconnect invokes this method.
    void review(const QString& action = QStringLiteral("install"));
    void confirm(bool accepted);
    // Called only after the real MachineBackend's authenticated daemon.hello.
    void connectionResult(bool connected);
    void cancel();
  signals:
    void consentRequested(const QString& description);
    void progress(const QString& message);
    void connectionRequested();
    void finished(bool success, const QString& message);

  private:
    void setState(State state, const QString& message);
    void fail(const QString& message);
    void complete(bool success, const QString& message);
    void fetch(const QUrl& url, qint64 limit, std::function<void(QByteArray)> done,
               bool archive = false, int redirects = 0);
    void command(const QJsonObject& description, bool transaction);
    void readProcess();
    void pumpArchive();
    void describe();
    [[nodiscard]] bool identityMatches(bool reconnect = false) const;
    QPointer<SshTunnel> tunnel_;
    DaemonReleaseChannel channel_;
    State state_{State::Idle};
    QNetworkAccessManager* network_;
    QPointer<QNetworkReply> reply_;
    QPointer<QProcess> process_;
    std::unique_ptr<QTemporaryDir> work_;
    std::unique_ptr<QFile> archive_;
    QJsonObject preflight_;
    std::optional<DaemonReleaseArtifact> artifact_;
    QByteArray manifest_;
    QByteArray output_;
    QString action_;
    QString fingerprint_;
    QString socket_;
    QString failureMessage_;
    bool cancelled_{false};
    bool transaction_{false};
    bool terminal_{false};
    bool commitSent_{false};
    bool rolledBack_{false};
    int generation_{0};
    quint64 sshGeneration_{0};
};
} // namespace ketplus::remote::ssh

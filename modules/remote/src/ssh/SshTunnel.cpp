#include "ssh/SshTunnel.h"
#include "ssh/SshProcess.h"
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

namespace ketplus::remote::ssh {
Q_LOGGING_CATEGORY(lcSsh, "ketplus.remote.ssh")

namespace {
// ssh_config tokens only: never an option ("-..."), whitespace or a shell metacharacter.
bool safeName(const QString& value, qsizetype maximum) {
    static const QRegularExpression allowed("^[A-Za-z0-9_][A-Za-z0-9@._%+:\\[\\]-]*$");
    return value.size() <= maximum && allowed.match(value).hasMatch();
}
} // namespace

// The flags every call shares: no prompt of any kind, no tty, and none of the user's
// multiplexing, forwarding, remote or local commands (they would change what runs and where).
// Calls that can authenticate log at ERROR, so a server banner never reaches stderr.
QStringList batchOptions(int connectTimeout, bool stdio, const QString& logLevel) {
    QStringList options;
    if (!stdio)
        options << "-n";
    return options + QStringList{"-T",
                                 "-o",
                                 "BatchMode=yes",
                                 "-o",
                                 "PasswordAuthentication=no",
                                 "-o",
                                 "KbdInteractiveAuthentication=no",
                                 "-o",
                                 "StrictHostKeyChecking=yes",
                                 "-o",
                                 "UpdateHostKeys=no",
                                 "-o",
                                 "CheckHostIP=no",
                                 "-o",
                                 "ControlMaster=no",
                                 "-o",
                                 "ControlPath=none",
                                 "-o",
                                 "ClearAllForwardings=yes",
                                 "-o",
                                 "RemoteCommand=none",
                                 "-o",
                                 "PermitLocalCommand=no",
                                 "-o",
                                 "ForwardAgent=no",
                                 "-o",
                                 "ForwardX11=no",
                                 "-o",
                                 "LogLevel=" + logLevel,
                                 "-o",
                                 QStringLiteral("ConnectTimeout=%1").arg(connectTimeout)};
}
// ProxyJump hops read the user's config, not these options: no askpass dialog on any hop.
QProcessEnvironment sshEnvironment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert("SSH_ASKPASS_REQUIRE", "never");
    return environment;
}

bool SshTarget::isValid() const {
    return safeName(alias, 255) && (user.isEmpty() || safeName(user, 64)) && port >= 0 &&
           port <= 65535;
}

QJsonObject SshTarget::toJson() const {
    QJsonObject value{{"alias", alias}};
    if (!user.isEmpty())
        value.insert("user", user);
    if (port > 0)
        value.insert("port", port);
    return value;
}

SshTarget SshTarget::fromJson(const QJsonObject& value) {
    return {value.value("alias").toString(), value.value("user").toString(),
            value.value("port").toInt()};
}

QString SshTunnel::defaultKnownHostsFile() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
           "/Ketsuite/KetPlus/ssh/known_hosts";
}

SshTunnel::SshTunnel(SshTarget target, Options options, QObject* parent)
    : QObject(parent), target_(std::move(target)), options_(std::move(options)),
      retry_(new QTimer(this)) {
    if (options_.knownHostsFile.isEmpty())
        options_.knownHostsFile = defaultKnownHostsFile();
    retry_->setSingleShot(true);
    connect(retry_, &QTimer::timeout, this, [this] {
        ++generation_;
        setState(State::Connecting);
        resolve();
    });
}

SshTunnel::~SshTunnel() {
    ++generation_;
    teardown();
}

void SshTunnel::setHostKeyPrompt(HostKeyPrompt prompt) { prompt_ = std::move(prompt); }

QLocalSocket* SshTunnel::device() const {
    return state_ == State::Connected ? socket_.data() : nullptr;
}

QByteArray SshTunnel::daemonCredential() const {
    return state_ == State::Connected ? credential_ : QByteArray{};
}

void SshTunnel::open() {
    retry_->stop();
    ++generation_;
    teardown();
    reconnects_ = 0;
    // Nothing from an earlier attempt describes this one.
    hostKeyStatus_ = HostKeyStatus::None;
    hostKey_ = {};
    certificate_ = false;
    fingerprint_.clear();
    remoteSocket_.clear();
    lastError_.clear();
    if (!target_.isValid()) {
        fail(State::Offline, "Invalid SSH host.");
        return;
    }
    setState(State::Connecting);
    resolve();
}

void SshTunnel::close() {
    retry_->stop();
    ++generation_;
    teardown();
    setState(State::Idle);
}

void SshTunnel::setState(State state) {
    if (state_ == state)
        return;
    state_ = state;
    qCInfo(lcSsh).noquote() << target_.alias << QVariant::fromValue(state).toString();
    emit stateChanged(state);
}

void SshTunnel::teardown() {
    // Callers bump generation_ first, so the signals this raises are ignored.
    credential_.fill('\0');
    credential_.clear();
    carried_ = false;
    // A QProcess destroyed while running warns and leaves the child behind. A running child is
    // killed and detached: it deletes itself once reaped, without blocking this thread, and it
    // outlives the tunnel if the tunnel is destroyed first.
    const auto stop = [](QProcess* process) {
        process->disconnect();
        if (process->state() == QProcess::NotRunning) {
            process->deleteLater();
            return;
        }
        process->setParent(nullptr);
        QObject::connect(process, &QProcess::finished, process, &QObject::deleteLater);
        process->kill();
    };
    if (step_)
        stop(step_);
    if (socket_) {
        socket_->abort();
        socket_->deleteLater();
    }
    if (peer_) {
        peer_->abort();
        peer_->deleteLater();
    }
    if (bridge_) {
        bridge_->close();
        bridge_->deleteLater();
    }
    if (forwarder_)
        stop(forwarder_);
}

void SshTunnel::run(const QString& program, const QStringList& arguments, StepDone done) {
    auto* process = new QProcess(this);
    auto* timer = new QTimer(process);
    timer->setSingleShot(true);
    step_ = process;
    const auto generation = generation_;
    auto finish = std::make_shared<StepDone>(std::move(done));
    connect(timer, &QTimer::timeout, process, [process] { process->kill(); });
    connect(process, &QProcess::finished, this,
            [this, process, generation, finish](int code, QProcess::ExitStatus status) {
                process->deleteLater();
                if (generation != generation_)
                    return;
                (*finish)(status == QProcess::NormalExit, code, process->readAllStandardOutput(),
                          process->readAllStandardError());
            });
    connect(process, &QProcess::errorOccurred, this,
            [this, process, generation, finish](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart)
                    return;
                process->deleteLater();
                if (generation == generation_)
                    (*finish)(false, -1, {}, {});
            });
    timer->start(options_.stepTimeoutMilliseconds);
    process->setProcessEnvironment(sshEnvironment());
    process->start(program, arguments);
}

QStringList SshTunnel::destinationOptions() const {
    auto arguments = options_.sshArguments;
    if (!target_.user.isEmpty())
        arguments << "-l" << target_.user;
    if (target_.port > 0)
        arguments << "-p" << QString::number(target_.port);
    return arguments;
}

QStringList SshTunnel::verifiedOptions(bool stdio) const {
    // ssh itself checks the key again against the same files, restricted to the verified type,
    // so a key that changes after the probe fails closed. A host trusted through a CA keeps
    // ssh's algorithm order, and ssh verifies the certificate it presents.
    auto files = knownFiles_;
    files.append(options_.knownHostsFile);
    auto options = destinationOptions() + batchOptions(options_.connectTimeoutSeconds, stdio) +
                   QStringList{"-o", "UserKnownHostsFile=" + quotedList(files),
                               "-o", "GlobalKnownHostsFile=/dev/null",
                               "-o", "ServerAliveInterval=15",
                               "-o", "ServerAliveCountMax=3"};
    if (!certificate_)
        options << "-o" << "HostKeyAlgorithms=" + algorithmsFor({hostKey_.type}).join(',');
    return options;
}

SshTunnel::State SshTunnel::classify(const QByteArray& err) {
    switch (classifySshError(err)) {
    case SshFailure::AuthFailed:
        return State::AuthFailed;
    case SshFailure::HostKeyChanged:
    case SshFailure::NoMatchingHostKeyType:
        return State::HostKeyMismatch;
    case SshFailure::Other:
        break;
    }
    return State::Offline;
}

void SshTunnel::fail(State state, const QString& error) {
    // Everything this attempt started is abandoned, including a pending accept callback.
    ++generation_;
    teardown();
    if (state == State::HostKeyMismatch)
        hostKeyStatus_ = HostKeyStatus::Mismatch;
    lastError_ = error;
    setState(state);
    if (state != State::Offline && state != State::DaemonSocketMissing)
        return;
    // Bounded backoff: doubling from the initial delay up to the maximum.
    const auto shift = qMin(reconnects_, 16);
    const auto delay = qMin<qint64>(qint64(options_.initialBackoffMilliseconds) << shift,
                                    options_.maximumBackoffMilliseconds);
    ++reconnects_;
    retry_->start(int(delay));
}

void SshTunnel::lost(const QString& error) { fail(State::Offline, error); }

void SshTunnel::resolve() {
    run(options_.sshProgram, destinationOptions() + QStringList{"-G", target_.alias},
        [this](bool finished, int code, const QByteArray& out, const QByteArray&) {
            if (!finished || code != 0) {
                fail(State::Offline, "The SSH configuration for this host could not be read.");
                return;
            }
            knownFiles_ = knownHostsFiles(parseResolvedConfig(out));
            probe({});
        });
}

} // namespace ketplus::remote::ssh

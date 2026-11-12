#include "ssh/SshTunnel.h"
#include <QDir>
#include <QFile>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTimer>

namespace ketplus::remote::ssh {
namespace {
Q_LOGGING_CATEGORY(lcSsh, "ketplus.remote.ssh")

// ssh_config tokens only: never an option ("-..."), whitespace or a shell metacharacter.
bool safeName(const QString& value, qsizetype maximum) {
    static const QRegularExpression allowed("^[A-Za-z0-9_][A-Za-z0-9@._%+:\\[\\]-]*$");
    return value.size() <= maximum && allowed.match(value).hasMatch();
}
// The flags every step shares: no prompt of any kind, no stdin, no tty.
QStringList batchOptions(int connectTimeout) {
    return {"-n", "-T",
            "-o", "BatchMode=yes",
            "-o", "PasswordAuthentication=no",
            "-o", "KbdInteractiveAuthentication=no",
            "-o", "StrictHostKeyChecking=yes",
            "-o", "UpdateHostKeys=no",
            "-o", "CheckHostIP=no",
            "-o", QStringLiteral("ConnectTimeout=%1").arg(connectTimeout)};
}
} // namespace

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

QIODevice* SshTunnel::device() const {
    return state_ == State::Connected ? socket_.data() : nullptr;
}

void SshTunnel::open() {
    retry_->stop();
    ++generation_;
    teardown();
    reconnects_ = 0;
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
    if (step_) {
        step_->kill();
        step_->deleteLater();
    }
    if (socket_) {
        socket_->abort();
        socket_->deleteLater();
    }
    if (forwarder_) {
        forwarder_->kill();
        forwarder_->deleteLater();
    }
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

QStringList SshTunnel::verifiedOptions() const {
    // ssh itself checks the key again against the same files, restricted to the verified type,
    // so a key that changes after the probe fails closed.
    auto files = knownFiles_;
    files.append(options_.knownHostsFile);
    return destinationOptions() + batchOptions(options_.connectTimeoutSeconds) +
           QStringList{"-o", "UserKnownHostsFile=" + quotedList(files),
                       "-o", "GlobalKnownHostsFile=/dev/null",
                       "-o", "HostKeyAlgorithms=" + algorithmsFor({hostKey_.type}).join(','),
                       "-o", "ServerAliveInterval=15",
                       "-o", "ServerAliveCountMax=3"};
}

SshTunnel::State SshTunnel::classify(const QByteArray& err) const {
    if (err.contains("Permission denied"))
        return State::AuthFailed;
    if (err.contains("REMOTE HOST IDENTIFICATION HAS CHANGED") ||
        err.contains("Host key verification failed"))
        return State::HostKeyMismatch;
    return State::Offline;
}

void SshTunnel::fail(State state, const QString& error) {
    // Everything this attempt started is abandoned, including a pending accept callback.
    ++generation_;
    teardown();
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

void SshTunnel::probe(const QStringList& algorithms) {
    if (!work_) {
        work_ = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/ketplus-ssh-XXXXXX");
        if (!work_->isValid() || work_->path().contains('"') || work_->path().contains('\'')) {
            work_.reset();
            fail(State::Offline, "Unable to create a private SSH directory.");
            return;
        }
        QFile::setPermissions(work_->path(),
                              QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    }
    // ssh hands each offered key to KnownHostsCommand; with no known_hosts file it then refuses
    // the host before authentication, so the probe sends nothing to the daemon or the account.
    const auto capture = work_->filePath("captured");
    const auto script = work_->filePath("capture.sh");
    QFile::remove(capture);
    QFile file(script);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
        file.write(QStringLiteral("printf '%s %s %s\\n' \"$1\" \"$2\" \"$3\" >> '%1'\n")
                       .arg(capture)
                       .toUtf8()) < 0) {
        fail(State::Offline, "Unable to create a private SSH directory.");
        return;
    }
    file.close();
    auto arguments = destinationOptions() + batchOptions(options_.connectTimeoutSeconds);
    arguments << "-o" << "UserKnownHostsFile=/dev/null" << "-o" << "GlobalKnownHostsFile=/dev/null"
              << "-o" << QStringLiteral("KnownHostsCommand=/bin/sh \"%1\" %H %t %K").arg(script);
    if (!algorithms.isEmpty())
        arguments << "-o" << "HostKeyAlgorithms=" + algorithms.join(',');
    arguments << target_.alias << "true";
    run(options_.sshProgram, arguments,
        [this, capture, algorithms](bool, int, const QByteArray&, const QByteArray& err) {
            QFile captured(capture);
            const auto keys = captured.open(QIODevice::ReadOnly) ? parseCaptured(captured.readAll())
                                                                 : QList<HostKey>{};
            if (keys.isEmpty()) {
                if (!algorithms.isEmpty()) {
                    // The host no longer offers any key type known for it.
                    hostKeyStatus_ = HostKeyStatus::Mismatch;
                    fail(State::HostKeyMismatch, "The host key changed.");
                } else if (err.toLower().contains("knownhostscommand")) {
                    fail(State::Offline, "OpenSSH 8.5 or later is required.");
                } else {
                    const auto state = classify(err);
                    fail(state == State::HostKeyMismatch ? State::Offline : state,
                         "The host could not be reached.");
                }
                return;
            }
            lookup(keys.last(), !algorithms.isEmpty());
        });
}

void SshTunnel::lookup(const HostKey& key, bool reprobed) {
    auto files = knownFiles_;
    files.append(options_.knownHostsFile);
    auto entries = std::make_shared<QHash<QString, QStringList>>();
    auto next = std::make_shared<std::function<void(qsizetype)>>();
    // ssh-keygen -F matches plain, bracketed and hashed names exactly as ssh does. The pending
    // step owns the chain; the chain itself holds it weakly.
    *next = [this, key, reprobed, files, entries, weak = std::weak_ptr(next)](qsizetype index) {
        while (index < files.size() && !QFile::exists(files.at(index)))
            ++index;
        if (index < files.size()) {
            run(options_.keygenProgram, {"-F", key.knownHostsName, "-f", files.at(index)},
                [entries, chain = weak.lock(), index](bool, int, const QByteArray& out,
                                                      const QByteArray&) {
                    const auto found = parseKnownEntries(out);
                    for (auto it = found.cbegin(); it != found.cend(); ++it)
                        (*entries)[it.key()].append(it.value());
                    (*chain)(index + 1);
                });
            return;
        }
        const auto known = entries->value(key.type);
        if (known.contains(key.base64)) {
            verified(key);
        } else if (!known.isEmpty() || (!entries->isEmpty() && reprobed)) {
            hostKeyStatus_ = HostKeyStatus::Mismatch;
            fingerprint_ = key.fingerprint();
            qCWarning(lcSsh).noquote() << target_.alias << "host key mismatch" << fingerprint_;
            fail(State::HostKeyMismatch, "The host key changed.");
        } else if (!entries->isEmpty()) {
            // Only other key types are known: ask for one of those before deciding.
            probe(algorithmsFor(entries->keys()));
        } else {
            hostKeyStatus_ = HostKeyStatus::Unknown;
            fingerprint_ = key.fingerprint();
            if (!prompt_) {
                fail(State::HostKeyRejected, "The host key was not accepted.");
                return;
            }
            const auto generation = generation_;
            QPointer<SshTunnel> self(this);
            prompt_(key, [self, generation, key](bool accept) {
                if (!self || generation != self->generation_)
                    return;
                QString error;
                if (!accept) {
                    self->fail(State::HostKeyRejected, "The host key was not accepted.");
                } else if (!recordHostKey(self->options_.knownHostsFile, key, &error)) {
                    self->fail(State::HostKeyRejected, error);
                } else {
                    qCInfo(lcSsh).noquote()
                        << self->target_.alias << "host key accepted" << key.fingerprint();
                    self->verified(key);
                }
            });
        }
    };
    (*next)(0);
}

void SshTunnel::verified(const HostKey& key) {
    hostKey_ = key;
    hostKeyStatus_ = HostKeyStatus::Verified;
    fingerprint_ = key.fingerprint();
    qCInfo(lcSsh).noquote() << target_.alias << "host key verified" << fingerprint_;
    locate();
}

void SshTunnel::locate() {
    run(options_.sshProgram,
        verifiedOptions() +
            QStringList{target_.alias, "exec /bin/sh -c '" + remoteSocketProbe() + "'"},
        [this](bool finished, int code, const QByteArray& out, const QByteArray& err) {
            bool present = false;
            QString path;
            if (!finished || code != 0 || !parseSocketProbe(out, &present, &path)) {
                fail(classify(err), "The host could not be reached.");
                return;
            }
            remoteSocket_ = path;
            if (!present) {
                fail(State::DaemonSocketMissing, "The KetPlus daemon is not running on the host.");
                return;
            }
            forward();
        });
}

void SshTunnel::forward() {
    const auto local = work_->filePath("daemon.sock");
    QFile::remove(local);
    auto* process = new QProcess(this);
    forwarder_ = process;
    const auto generation = generation_;
    connect(process, &QProcess::finished, this, [this, process, generation] {
        process->deleteLater();
        if (generation != generation_)
            return;
        const auto state = classify(process->readAllStandardError());
        fail(state, state == State::Offline ? "The SSH connection closed."
                                            : "The host refused the SSH connection.");
    });
    connect(process, &QProcess::errorOccurred, this, [this, generation](QProcess::ProcessError e) {
        if (e == QProcess::FailedToStart && generation == generation_)
            fail(State::Offline, "Unable to start ssh.");
    });
    process->start(options_.sshProgram,
                   verifiedOptions() + QStringList{"-N", "-o", "ExitOnForwardFailure=yes", "-o",
                                                   "StreamLocalBindUnlink=yes", "-o",
                                                   "StreamLocalBindMask=0177", "-L",
                                                   local + ':' + remoteSocket_, target_.alias});
    attach(qMax(1, options_.stepTimeoutMilliseconds / 50));
}

void SshTunnel::attach(int remaining) {
    // ssh creates the local socket once it has authenticated; until then connecting fails.
    const auto generation = generation_;
    auto* socket = new QLocalSocket(this);
    connect(socket, &QLocalSocket::connected, this, [this, socket, generation] {
        if (generation != generation_) {
            socket->abort();
            socket->deleteLater();
            return;
        }
        socket_ = socket;
        connect(socket, &QLocalSocket::disconnected, this, [this, socket, generation] {
            if (generation == generation_ && socket_ == socket)
                lost("The daemon connection closed.");
        });
        reconnects_ = 0;
        lastError_.clear();
        setState(State::Connected);
    });
    connect(socket, &QLocalSocket::errorOccurred, this, [this, socket, generation, remaining] {
        socket->deleteLater();
        if (generation != generation_ || !forwarder_)
            return;
        if (remaining <= 1) {
            fail(State::Offline, "The SSH forward did not start.");
            return;
        }
        QTimer::singleShot(50, this, [this, generation, remaining] {
            if (generation == generation_ && forwarder_)
                attach(remaining - 1);
        });
    });
    socket->connectToServer(work_->filePath("daemon.sock"));
}

} // namespace ketplus::remote::ssh

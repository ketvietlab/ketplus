#include "ssh/SshProcess.h"
#include "ssh/SshTunnel.h"
#include <QDir>
#include <QFile>
#include <QPointer>
#include <QTemporaryDir>

// Host-key steps of SshTunnel: the KnownHostsCommand probe, the known_hosts lookup and the
// accept prompt (docs/ssh-transport.md, steps 2 and 3).
namespace ketplus::remote::ssh {

void SshTunnel::probe(const QStringList& algorithms, const HostKey& offered) {
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
    // The probe never authenticates (no banner is sent before that), and INFO keeps ssh's
    // "no matching host key type" line.
    auto arguments =
        destinationOptions() + batchOptions(options_.connectTimeoutSeconds, false, "INFO");
    arguments << "-o" << "UserKnownHostsFile=/dev/null" << "-o" << "GlobalKnownHostsFile=/dev/null"
              << "-o" << QStringLiteral("KnownHostsCommand=/bin/sh \"%1\" %H %t %K").arg(script);
    if (!algorithms.isEmpty())
        arguments << "-o" << "HostKeyAlgorithms=" + algorithms.join(',');
    arguments << target_.alias << "true";
    run(options_.sshProgram, arguments,
        [this, capture, algorithms, offered](bool, int, const QByteArray&, const QByteArray& err) {
            QFile captured(capture);
            const auto keys = captured.open(QIODevice::ReadOnly) ? parseCaptured(captured.readAll())
                                                                 : QList<HostKey>{};
            if (keys.isEmpty()) {
                if (!algorithms.isEmpty() &&
                    classifySshError(err) == SshFailure::NoMatchingHostKeyType) {
                    // The host no longer offers any key type known for it.
                    mismatch(offered, "The host key changed.");
                } else if (!algorithms.isEmpty()) {
                    // Unreachable or timed out: nothing was learned about the key.
                    fail(State::Offline, "The host could not be reached.");
                } else if (err.toLower().contains("knownhostscommand")) {
                    fail(State::Offline, "OpenSSH 8.5 or later is required.");
                } else {
                    // This probe trusts no file, so ssh always ends with a host-key refusal.
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
    auto entries = std::make_shared<KnownEntries>();
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
                    entries->merge(parseKnownEntries(out));
                    (*chain)(index + 1);
                });
            return;
        }
        const auto known = entries->keys.value(key.type);
        if (entries->revoked.contains(key.base64)) {
            mismatch(key, "The host key is revoked.");
        } else if (known.contains(key.base64)) {
            verified(key, false);
        } else if (!known.isEmpty() || (!entries->keys.isEmpty() && reprobed)) {
            mismatch(key, "The host key changed.");
        } else if (entries->certificateAuthority) {
            verified(key, true);
        } else if (!entries->keys.isEmpty()) {
            // Only other key types are known: ask for one of those before deciding.
            probe(algorithmsFor(entries->keys.keys()), key);
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
                    self->verified(key, false);
                }
            });
        }
    };
    (*next)(0);
}

void SshTunnel::mismatch(const HostKey& key, const QString& error) {
    hostKeyStatus_ = HostKeyStatus::Mismatch;
    // The offending key, never one an earlier attempt verified.
    fingerprint_ = key.fingerprint();
    qCWarning(lcSsh).noquote() << target_.alias << "host key mismatch" << fingerprint_;
    fail(State::HostKeyMismatch, error);
}

void SshTunnel::verified(const HostKey& key, bool certificate) {
    hostKey_ = key;
    certificate_ = certificate;
    // A @cert-authority line only names who may sign; ssh decides on the certificate itself,
    // so the CA path stays unverified until the first verified call succeeds (locate()).
    hostKeyStatus_ = certificate ? HostKeyStatus::None : HostKeyStatus::Verified;
    fingerprint_ = key.fingerprint();
    qCInfo(lcSsh).noquote() << target_.alias
                            << (certificate ? "host key trusted by a CA" : "host key verified")
                            << fingerprint_;
    locate();
}

} // namespace ketplus::remote::ssh

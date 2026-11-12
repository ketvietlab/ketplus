#include "ssh/SshHostKeys.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>

namespace ketplus::remote::ssh {
namespace {
QString expandHome(QString path) {
    if (path == "~" || path.startsWith("~/"))
        path = QDir::homePath() + path.mid(1);
    return path;
}
bool safeToken(const QString& value) {
    static const QRegularExpression allowed("^[A-Za-z0-9@._:\\[\\]*?!,|=+/-]+$");
    return !value.isEmpty() && value.size() <= 16384 && allowed.match(value).hasMatch();
}
} // namespace

QString HostKey::fingerprint() const {
    const auto blob = QByteArray::fromBase64(base64.toLatin1());
    if (blob.isEmpty())
        return {};
    return "SHA256:" +
           QString::fromLatin1(QCryptographicHash::hash(blob, QCryptographicHash::Sha256)
                                   .toBase64(QByteArray::OmitTrailingEquals));
}

QHash<QString, QString> parseResolvedConfig(const QByteArray& output) {
    QHash<QString, QString> config;
    for (const auto& raw : output.split('\n')) {
        const auto line = QString::fromUtf8(raw).trimmed();
        const auto space = line.indexOf(' ');
        if (space <= 0)
            continue;
        const auto key = line.left(space).toLower();
        if (!config.contains(key))
            config.insert(key, line.mid(space + 1).trimmed());
    }
    return config;
}

QStringList knownHostsFiles(const QHash<QString, QString>& config) {
    QStringList files;
    for (const auto* key : {"userknownhostsfile", "globalknownhostsfile"}) {
        const auto tokens = config.value(QLatin1String(key)).split(' ', Qt::SkipEmptyParts);
        for (qsizetype i = 0; i < tokens.size(); ++i) {
            auto path = tokens.at(i);
            if (path.startsWith('"')) {
                // A quoted path, as ssh_config writes one.
                while (!path.endsWith('"') && i + 1 < tokens.size())
                    path += ' ' + tokens.at(++i);
                path = path.mid(1, path.size() - (path.endsWith('"') ? 2 : 1));
            } else {
                // The longest run of tokens that names an existing file is one path.
                for (qsizetype last = tokens.size() - 1; last > i; --last) {
                    const auto joined = tokens.mid(i, last - i + 1).join(' ');
                    if (QFileInfo::exists(expandHome(joined))) {
                        path = joined;
                        i = last;
                        break;
                    }
                }
            }
            if (path != "none" && path != "/dev/null")
                files.append(expandHome(path));
        }
    }
    return files;
}

QList<HostKey> parseCaptured(const QByteArray& output) {
    QList<HostKey> keys;
    for (const auto& raw : output.split('\n')) {
        const auto fields = QString::fromUtf8(raw).split(' ', Qt::SkipEmptyParts);
        // ssh asks once with NONE before it has a key, to order its algorithms.
        if (fields.size() != 3 || fields.at(1) == "NONE" || !safeToken(fields.at(0)) ||
            !safeToken(fields.at(1)) || !safeToken(fields.at(2)))
            continue;
        HostKey key{fields.at(0), fields.at(1), fields.at(2)};
        if (!key.fingerprint().isEmpty())
            keys.append(key);
    }
    return keys;
}

void KnownEntries::merge(const KnownEntries& other) {
    for (auto it = other.keys.cbegin(); it != other.keys.cend(); ++it)
        keys[it.key()].append(it.value());
    revoked.append(other.revoked);
    certificateAuthority |= other.certificateAuthority;
}

KnownEntries parseKnownEntries(const QByteArray& output) {
    KnownEntries entries;
    for (const auto& raw : output.split('\n')) {
        const auto line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        auto fields = line.split(' ', Qt::SkipEmptyParts);
        if (fields.first().startsWith('@')) {
            // "@marker hosts type key": a revoked key never verifies a host, a CA line is no
            // host key but means ssh can verify a certificate the host presents.
            if (fields.first() == "@revoked" && fields.size() >= 4)
                entries.revoked.append(fields.at(3));
            else if (fields.first() == "@cert-authority" && fields.size() >= 4)
                entries.certificateAuthority = true;
            continue;
        }
        if (fields.size() >= 3)
            entries.keys[fields.at(1)].append(fields.at(2));
    }
    return entries;
}

SshFailure classifySshError(const QByteArray& err) {
    static const QRegularExpression changed(
        "^(@\\s+WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!\\s+@|"
        "Host key verification failed\\.)$");
    static const QRegularExpression denied("^\\S+: Permission denied \\([A-Za-z0-9,@._-]+\\)\\.$");
    static const QRegularExpression negotiate(
        "^Unable to negotiate with \\S+ port \\d+: no matching host key type found\\.");
    auto failure = SshFailure::Other;
    for (const auto& raw : err.split('\n')) {
        const auto line = QString::fromUtf8(raw).trimmed();
        if (changed.match(line).hasMatch())
            return SshFailure::HostKeyChanged;
        if (negotiate.match(line).hasMatch())
            failure = SshFailure::NoMatchingHostKeyType;
        else if (denied.match(line).hasMatch() && failure == SshFailure::Other)
            failure = SshFailure::AuthFailed;
    }
    return failure;
}

QStringList algorithmsFor(const QStringList& keyTypes) {
    QStringList algorithms;
    for (const auto& type : keyTypes) {
        if (type == "ssh-rsa")
            algorithms << "rsa-sha2-512" << "rsa-sha2-256";
        else
            algorithms << type;
    }
    algorithms.removeDuplicates();
    return algorithms;
}

QString quotedList(const QStringList& paths) {
    QStringList quoted;
    for (const auto& path : paths)
        quoted.append('"' + path + '"');
    return quoted.join(' ');
}

bool recordHostKey(const QString& file, const HostKey& key, QString* error) {
    const auto fail = [&](const QString& message) {
        if (error)
            *error = message;
        return false;
    };
    if (!key.isValid() || !safeToken(key.knownHostsName))
        return fail("Invalid host key.");
    const QFileInfo info(file);
    if (!QDir().mkpath(info.absolutePath()) ||
        !QFile::setPermissions(info.absolutePath(),
                               QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner))
        return fail("Unable to create the KetPlus known_hosts directory.");
    QByteArray contents;
    if (QFile existing(file); existing.exists()) {
        if (!existing.open(QIODevice::ReadOnly))
            return fail("Unable to read the KetPlus known_hosts file.");
        contents = existing.readAll();
        if (!contents.isEmpty() && !contents.endsWith('\n'))
            contents.append('\n');
    }
    contents.append(
        QStringLiteral("%1 %2 %3\n").arg(key.knownHostsName, key.type, key.base64).toUtf8());
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) ||
        !out.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        out.write(contents) != contents.size() || !out.commit())
        return fail("Unable to write the KetPlus known_hosts file.");
    return true;
}

QString remoteSocketProbe() {
    // No single quote anywhere: the caller wraps it in sh -c '...'. Each setting comes from the
    // environment, then the unit's daemon.env, then the unit's default.
    return QStringLiteral(
        "e=\"$HOME/.config/ketplus/daemon.env\"; "
        "v() { if [ -r \"$e\" ]; then sed -n \"s/^[[:space:]]*$1=//p\" \"$e\" | tail -n 1 | "
        "tr -d \"\\\"\"; fi; }; "
        "s=\"${KETPLUS_DAEMON_SOCKET:-}\"; if [ -z \"$s\" ]; then s=$(v KETPLUS_DAEMON_SOCKET); "
        "fi; "
        "d=\"${KETPLUS_DAEMON_DATA_DIR:-}\"; if [ -z \"$d\" ]; then d=$(v "
        "KETPLUS_DAEMON_DATA_DIR); "
        "fi; if [ -z \"$d\" ]; then d=\"${XDG_DATA_HOME:-$HOME/.local/share}/Ketsuite/KetPlus\"; "
        "fi; "
        "if [ -z \"$s\" ]; then r=\"${XDG_RUNTIME_DIR:-}\"; "
        "if [ -z \"$r\" ] && [ -d \"/run/user/$(id -u)\" ]; then r=\"/run/user/$(id -u)\"; fi; "
        "if [ -z \"$r\" ]; then r=\"$d\"; fi; s=\"$r/ketplus-daemon-v0\"; fi; "
        "k=\"${KETPLUS_DAEMON_TOKEN_FILE:-}\"; if [ -z \"$k\" ]; then "
        "k=$(v KETPLUS_DAEMON_TOKEN_FILE); fi; "
        "if [ -z \"$k\" ]; then k=\"$d/daemon/local.token\"; fi; "
        "if [ -S \"$s\" ]; then t=present; else t=missing; fi; "
        "printf \"ketplus-socket %s %s\\n\" \"$t\" \"$s\"; "
        "if [ \"$t\" = present ]; then "
        "if [ ! -f \"$k\" ] || [ ! -s \"$k\" ] || [ \"$(wc -c < \"$k\" | tr -d \" \")\" -gt 4096 "
        "]; then "
        "echo ketplus-token-missing; else "
        "m=$(ls -lnL \"$k\" | cut -c2-10); o=$(ls -lnL \"$k\" | awk \"{print \\$3}\"); "
        "case \"$m\" in ???------) p=yes;; *) p=no;; esac; "
        "if [ \"$p\" = yes ] && [ \"$o\" = \"$(id -u)\" ]; then "
        "printf \"ketplus-token %s\\n\" \"$(base64 < \"$k\" | tr -d \"\\n\")\"; "
        "else echo ketplus-token-permissive; fi; fi; fi");
}

bool parseDaemonProbe(const QByteArray& output, DaemonProbe* probe) {
    static const QRegularExpression socket("^ketplus-socket (present|missing) (/[^\\s:]{1,1023})$");
    static const QRegularExpression token("^ketplus-token ([A-Za-z0-9+/]{1,8192}={0,2})$");
    bool found = false;
    DaemonProbe result;
    for (const auto& raw : output.split('\n')) {
        const auto line = QString::fromUtf8(raw).trimmed();
        if (const auto match = socket.match(line); match.hasMatch() && !found) {
            found = true;
            result.present = match.captured(1) == "present";
            result.socketPath = match.captured(2);
        } else if (const auto value = token.match(line); value.hasMatch() && found) {
            result.credential = QByteArray::fromBase64(value.captured(1).toLatin1());
            result.token = result.credential.isEmpty() ? DaemonProbe::Token::Missing
                                                       : DaemonProbe::Token::Present;
        } else if (line == "ketplus-token-permissive" && found) {
            result.token = DaemonProbe::Token::Permissive;
        }
    }
    if (found)
        *probe = std::move(result);
    return found;
}

} // namespace ketplus::remote::ssh

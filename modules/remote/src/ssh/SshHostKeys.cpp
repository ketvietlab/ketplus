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
    for (const auto* key : {"userknownhostsfile", "globalknownhostsfile"})
        for (const auto& path : config.value(QLatin1String(key)).split(' ', Qt::SkipEmptyParts))
            if (path != "none" && path != "/dev/null")
                files.append(expandHome(path));
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

QHash<QString, QStringList> parseKnownEntries(const QByteArray& output) {
    QHash<QString, QStringList> entries;
    for (const auto& raw : output.split('\n')) {
        const auto line = QString::fromUtf8(raw).trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;
        auto fields = line.split(' ', Qt::SkipEmptyParts);
        // A revoked key never verifies a host; a CA line is not a host key.
        if (!fields.isEmpty() && fields.first().startsWith('@')) {
            if (fields.first() != "@revoked" && fields.first() != "@cert-authority")
                continue;
            continue;
        }
        if (fields.size() >= 3)
            entries[fields.at(1)].append(fields.at(2));
    }
    return entries;
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
    // No single quote anywhere: the caller wraps it in sh -c '...'.
    return QStringLiteral(
        "s=\"${KETPLUS_DAEMON_SOCKET:-}\"; e=\"$HOME/.config/ketplus/daemon.env\"; "
        "if [ -z \"$s\" ] && [ -r \"$e\" ]; then "
        "s=$(sed -n \"s/^[[:space:]]*KETPLUS_DAEMON_SOCKET=//p\" \"$e\" | tail -n 1 | "
        "tr -d \"\\\"\"); fi; "
        "if [ -z \"$s\" ]; then r=\"${XDG_RUNTIME_DIR:-}\"; "
        "if [ -z \"$r\" ] && [ -d \"/run/user/$(id -u)\" ]; then r=\"/run/user/$(id -u)\"; fi; "
        "if [ -z \"$r\" ]; then "
        "r=\"${KETPLUS_DAEMON_DATA_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/Ketsuite/KetPlus}\"; "
        "fi; s=\"$r/ketplus-daemon-v0\"; fi; "
        "if [ -S \"$s\" ]; then t=present; else t=missing; fi; "
        "printf \"ketplus-socket %s %s\\n\" \"$t\" \"$s\"");
}

bool parseSocketProbe(const QByteArray& output, bool* present, QString* path) {
    static const QRegularExpression line("^ketplus-socket (present|missing) (/[^\\s:]{1,1023})$");
    for (const auto& raw : output.split('\n')) {
        const auto match = line.match(QString::fromUtf8(raw).trimmed());
        if (!match.hasMatch())
            continue;
        *present = match.captured(1) == "present";
        *path = match.captured(2);
        return true;
    }
    return false;
}

} // namespace ketplus::remote::ssh

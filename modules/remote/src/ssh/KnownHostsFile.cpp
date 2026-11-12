#include "ssh/KnownHostsFile.h"

#include <QByteArrayList>
#include <QCryptographicHash>
#include <QFile>
#include <QMessageAuthenticationCode>
#include <QSaveFile>

namespace ketplus::remote::ssh {
namespace {

// "|1|<base64 salt>|<base64 HMAC-SHA1(salt, name)>", as HashKnownHosts writes it.
bool hashedNameMatches(const QByteArray& field, const QByteArray& name) {
    const QByteArrayList parts = field.split('|');
    if (parts.size() != 4 || !parts.at(0).isEmpty() || parts.at(1) != "1")
        return false;
    const QByteArray salt = QByteArray::fromBase64(parts.at(2));
    const QByteArray hash = QByteArray::fromBase64(parts.at(3));
    if (salt.isEmpty() || hash.isEmpty())
        return false;
    return QMessageAuthenticationCode::hash(name, salt, QCryptographicHash::Sha1) == hash;
}

enum class LineMatch { None, Whole, Partial };

// What removing `name` does to one line; `kept` receives the line's remaining names.
LineMatch matchLine(const QByteArray& line, const QByteArray& name, QByteArray* kept = nullptr) {
    const QByteArray trimmed = line.trimmed();
    // A marker line (@revoked, @cert-authority) is a trust decision of its own, never removed.
    if (trimmed.isEmpty() || trimmed.startsWith('#') || trimmed.startsWith('@'))
        return LineMatch::None;
    qsizetype space = 0;
    while (space < trimmed.size() && trimmed.at(space) != ' ' && trimmed.at(space) != '\t')
        ++space;
    if (space == 0 || space == trimmed.size())
        return LineMatch::None;
    const QByteArray names = trimmed.left(space);
    if (names.startsWith('|'))
        return hashedNameMatches(names, name) ? LineMatch::Whole : LineMatch::None;
    QByteArrayList rest;
    bool found = false;
    for (const QByteArray& entry : names.split(',')) {
        if (entry == name)
            found = true;
        else if (!entry.isEmpty())
            rest.append(entry);
    }
    if (!found)
        return LineMatch::None;
    if (rest.isEmpty())
        return LineMatch::Whole;
    if (kept)
        *kept = rest.join(',') + trimmed.mid(space);
    return LineMatch::Partial;
}

bool readLines(const QString& file, QByteArrayList* lines, bool* exists) {
    QFile in(file);
    *exists = in.exists();
    if (!*exists)
        return true;
    if (!in.open(QIODevice::ReadOnly))
        return false;
    *lines = in.readAll().split('\n');
    // A trailing newline leaves one empty element; it is written back by the join below.
    if (!lines->isEmpty() && lines->last().isEmpty())
        lines->removeLast();
    return true;
}

} // namespace

bool hasKnownHost(const QString& file, const QString& knownHostsName) {
    if (knownHostsName.isEmpty())
        return false;
    QByteArrayList lines;
    bool exists = false;
    if (!readLines(file, &lines, &exists) || !exists)
        return false;
    const QByteArray name = knownHostsName.toUtf8();
    for (const QByteArray& line : std::as_const(lines))
        if (matchLine(line, name) != LineMatch::None)
            return true;
    return false;
}

int removeKnownHost(const QString& file, const QString& knownHostsName, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error)
            *error = message;
        return -1;
    };
    if (knownHostsName.isEmpty())
        return 0;
    QByteArrayList lines;
    bool exists = false;
    if (!readLines(file, &lines, &exists))
        return fail(QStringLiteral("Unable to read the KetPlus known_hosts file."));
    if (!exists)
        return 0;
    const QByteArray name = knownHostsName.toUtf8();
    QByteArrayList next;
    int changed = 0;
    for (const QByteArray& line : std::as_const(lines)) {
        QByteArray kept;
        switch (matchLine(line, name, &kept)) {
        case LineMatch::None:
            next.append(line);
            break;
        case LineMatch::Partial:
            next.append(kept);
            ++changed;
            break;
        case LineMatch::Whole:
            ++changed;
            break;
        }
    }
    if (changed == 0)
        return 0;
    QByteArray contents = next.join('\n');
    if (!contents.isEmpty())
        contents.append('\n');
    QSaveFile out(file);
    if (!out.open(QIODevice::WriteOnly) ||
        !out.setPermissions(QFile::ReadOwner | QFile::WriteOwner) ||
        out.write(contents) != contents.size() || !out.commit())
        return fail(QStringLiteral("Unable to write the KetPlus known_hosts file."));
    return changed;
}

} // namespace ketplus::remote::ssh

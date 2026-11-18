#include "terminal/RunCommand.h"

#include <QStringList>

namespace ketplus {
namespace {

QString posixQuote(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(value);
}

bool hasLineBreakOrControl(const QString& value) {
    for (const QChar character : value) {
        if (character.unicode() < 0x20 || character.unicode() == 0x7f)
            return true;
    }
    return false;
}

// PowerShell reads ' and the typographic single quotes as quote characters inside '...'.
bool isPowerShellQuote(const QChar character) {
    switch (character.unicode()) {
    case 0x27:   // '
    case 0x2018: // left single quotation mark
    case 0x2019: // right single quotation mark
    case 0x201A: // single low-9 quotation mark
    case 0x201B: // single high-reversed-9 quotation mark
        return true;
    default:
        return false;
    }
}

QString powerShellQuote(const QString& value) {
    QString quoted = QStringLiteral("'");
    for (const QChar character : value) {
        quoted += character;
        if (isPowerShellQuote(character))
            quoted += character;
    }
    return quoted + QLatin1Char('\'');
}

// Names cmd expands on its own, besides the environment.
bool cmdDynamicVariable(const QString& name) {
    static const QStringList names{
        QStringLiteral("cd"),         QStringLiteral("date"),
        QStringLiteral("time"),       QStringLiteral("random"),
        QStringLiteral("errorlevel"), QStringLiteral("cmdextversion"),
        QStringLiteral("cmdcmdline"), QStringLiteral("highestnumanodenumber")};
    return names.contains(name.toLower());
}

// True when some `%name%` pair in `value` would be expanded by cmd. Any two % are treated as a
// pair, and `%name:...%` (substring and replace forms) counts by its name.
bool cmdWouldExpand(const QString& value, const QProcessEnvironment& environment) {
    // cmd variable names are case-insensitive; QProcessEnvironment's are not on every host.
    QStringList defined;
    for (const QString& key : environment.keys())
        defined.append(key.toLower());
    for (qsizetype open = value.indexOf(QLatin1Char('%')); open >= 0;
         open = value.indexOf(QLatin1Char('%'), open + 1)) {
        for (qsizetype close = value.indexOf(QLatin1Char('%'), open + 1); close >= 0;
             close = value.indexOf(QLatin1Char('%'), close + 1)) {
            QString name = value.mid(open + 1, close - open - 1);
            if (const qsizetype colon = name.indexOf(QLatin1Char(':')); colon >= 0)
                name.truncate(colon);
            if (!name.isEmpty() && (defined.contains(name.toLower()) || cmdDynamicVariable(name)))
                return true;
        }
    }
    return false;
}

} // namespace

ShellKind shellKindFor(const QString& shellPath) {
    // Either separator, whatever the host: the path may be a Windows one.
    QString name = shellPath
                       .mid(qMax(shellPath.lastIndexOf(QLatin1Char('/')),
                                 shellPath.lastIndexOf(QLatin1Char('\\'))) +
                            1)
                       .toLower();
    if (name.endsWith(QLatin1String(".exe")))
        name.chop(4);
    if (name == QLatin1String("powershell") || name == QLatin1String("pwsh"))
        return ShellKind::PowerShell;
    if (name == QLatin1String("cmd"))
        return ShellKind::Cmd;
    return ShellKind::Posix;
}

RunCommand runCommandFor(const ShellKind kind, const QString& interpreter,
                         const QString& absolutePath, const bool directlyExecutable,
                         const QProcessEnvironment& environment) {
    if (hasLineBreakOrControl(absolutePath))
        return {{},
                QStringLiteral("The file name has a control character the terminal cannot "
                               "type safely.")};
    switch (kind) {
    case ShellKind::PowerShell:
        return {QStringLiteral("& %1").arg(powerShellQuote(absolutePath)), {}};
    case ShellKind::Cmd:
        if (absolutePath.contains(QLatin1Char('"')) || cmdWouldExpand(absolutePath, environment))
            return {{},
                    QStringLiteral("cmd.exe cannot take this file name literally (a double "
                                   "quote or a %variable%). Use PowerShell or rename it.")};
        return {QStringLiteral("\"%1\"").arg(absolutePath), {}};
    case ShellKind::Posix:
        break;
    }
    if (directlyExecutable)
        return {posixQuote(absolutePath), {}};
    const QString shell = interpreter.isEmpty() ? QStringLiteral("/bin/sh") : interpreter;
    return {QStringLiteral("%1 %2").arg(posixQuote(shell), posixQuote(absolutePath)), {}};
}

} // namespace ketplus

#include "preview/MermaidLaunch.h"

#include <QDir>
#include <QFileInfo>

namespace ketplus {
namespace {

// Inside double quotes cmd takes & ^ | < > ( ) and spaces literally. It still expands %VAR%
// there, and a double quote cannot be escaped, so those are refused rather than guessed at.
bool quotableForCmd(const QString& value) {
    for (const QChar character : value) {
        if (character == QLatin1Char('%') || character == QLatin1Char('"') ||
            character == QLatin1Char('\n') || character == QLatin1Char('\r') ||
            character == QChar::Null)
            return false;
    }
    return true;
}

QString directScript(const QString& shim) {
    const QDir directory = QFileInfo(shim).absoluteDir();
    const QString script =
        directory.filePath(QStringLiteral("node_modules/@mermaid-js/mermaid-cli/src/cli.js"));
    return QFileInfo(script).isFile() ? script : QString();
}

} // namespace

QString cmdCommandLine(const QString& program, const QStringList& arguments, bool* ok) {
    QString line;
    bool valid = quotableForCmd(program);
    // cmd.exe wants backslashes in the program path; Qt hands out `C:/...` (whatever the host).
    QString nativeProgram = program;
    nativeProgram.replace(QLatin1Char('/'), QLatin1Char('\\'));
    line += QLatin1Char('"') + nativeProgram + QLatin1Char('"');
    for (const QString& argument : arguments) {
        valid = valid && quotableForCmd(argument);
        line += QStringLiteral(" \"") + argument + QLatin1Char('"');
    }
    if (ok != nullptr)
        *ok = valid;
    return valid ? line : QString();
}

MermaidLaunch mermaidWindowsLaunch(const QString& shellPath, const QString& shim,
                                   const QStringList& arguments,
                                   const QStringList& nodeCandidates) {
    MermaidLaunch launch;
    const QString script = directScript(shim);
    if (!script.isEmpty()) {
        for (const QString& node : nodeCandidates) {
            // Only a real executable: a node.cmd/.bat wrapper (version managers) would put
            // cmd.exe, and its argument parsing, back in between.
            if (node.endsWith(QLatin1String(".exe"), Qt::CaseInsensitive) &&
                QFileInfo(node).isFile()) {
                launch.program = node;
                launch.arguments = QStringList{script} + arguments;
                return launch;
            }
        }
    }
    bool ok = false;
    const QString line = cmdCommandLine(shim, arguments, &ok);
    if (!ok) {
        launch.error = QStringLiteral(
            "Mermaid cannot be started through cmd.exe because a path contains %, a double "
            "quote or a line break. Install Node.js or move the file.");
        return launch;
    }
    launch.program = shellPath;
    // /D skips AutoRun commands, /V:OFF turns delayed expansion off even if the registry turns
    // it on (so `!` in a path stays literal), and /S makes cmd remove exactly the outer quotes.
    launch.nativeArguments = QStringLiteral("/D /V:OFF /S /C \"%1\"").arg(line);
    return launch;
}

} // namespace ketplus

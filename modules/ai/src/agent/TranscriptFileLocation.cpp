#include "agent/TranscriptFileLocation.h"

#include <QDir>
#include <QRegularExpression>

#include <utility>

namespace ketplus::agent_message {

QString unquoteShellToken(QString value) {
    value = value.trimmed();
    while (value.size() >= 2 && ((value.startsWith(u'\'') && value.endsWith(u'\'')) ||
                                 (value.startsWith(u'"') && value.endsWith(u'"'))))
        value = value.mid(1, value.size() - 2).trimmed();
    return value;
}

QString pathOperand(QString value) {
    value = unquoteShellToken(std::move(value));
    while (value.endsWith(u',') || value.endsWith(u')') || value.endsWith(u'\'') ||
           value.endsWith(u'"'))
        value.chop(1);
    return value;
}

namespace {

QString expandHome(const QString& path) {
    return path.startsWith(QStringLiteral("~/")) ? QDir::home().filePath(path.mid(2)) : path;
}

} // namespace

QString fileLocation(const QString& path, const int line, const QString& command,
                     const QString& cwd) {
    if (path.isEmpty())
        return {};
    QString absolute = expandHome(path);
    if (QDir::isRelativePath(absolute)) {
        static const QRegularExpression leadingCd(
            R"((?:^|-[a-z]*c\s+["']?)\s*cd\s+((?:"[^"]+")|(?:'[^']+')|[^\s&;]+)\s*&&)");
        const auto match = leadingCd.match(command);
        QString base = expandHome(match.hasMatch() ? unquoteShellToken(match.captured(1)) : cwd);
        if (QDir::isRelativePath(base))
            base = cwd.isEmpty() || base.isEmpty() ? QString{} : QDir(cwd).filePath(base);
        if (base.isEmpty())
            return {};
        absolute = QDir(base).filePath(absolute);
    }
    absolute = QDir::cleanPath(absolute);
    return line > 1 ? QStringLiteral("%1:%2").arg(absolute).arg(line) : absolute;
}

} // namespace ketplus::agent_message

#include "agent/ChatMarkdown.h"

#include <QRegularExpression>
#include <QSet>
#include <QStringList>
#include <QUrl>

namespace ketplus::chat_markdown {
namespace {

bool asciiLetter(const QChar character) {
    const char16_t code = character.unicode();
    return (code >= u'a' && code <= u'z') || (code >= u'A' && code <= u'Z');
}

qsizetype indentation(const QString& line) {
    qsizetype columns = 0;
    for (const QChar character : line) {
        if (character == u' ')
            ++columns;
        else if (character == u'\t')
            columns += 4 - columns % 4;
        else
            break;
    }
    return columns;
}

bool listItem(const QString& line) {
    static const QRegularExpression marker(
        QStringLiteral(R"(^\s*(?:[-+*]|\d{1,9}[.)])(?:[ \t]|$))"));
    return marker.match(line).hasMatch();
}

// Whether a '<' at index opens something Qt's importer would parse as raw HTML: a tag, closing
// tag, comment, declaration or processing instruction.
bool opensHtml(const QString& text, const qsizetype index) {
    const auto at = [&text](const qsizetype position) {
        return position < text.size() ? text.at(position) : QChar();
    };
    const QChar next = at(index + 1);
    if (asciiLetter(next) || next == u'!' || next == u'?')
        return true;
    return next == u'/' && asciiLetter(at(index + 2));
}

bool autolinkAt(const QString& text, const qsizetype index) {
    static const QRegularExpression uri(
        QStringLiteral(R"(<[A-Za-z][A-Za-z0-9+.\-]{1,31}:[^\x00-\x20<>]*>)"));
    static const QRegularExpression email(QStringLiteral(
        R"(<[A-Za-z0-9.!#$%&'*+/=?^_`{|}~\-]+@[A-Za-z0-9](?:[A-Za-z0-9\-]{0,61}[A-Za-z0-9])?)"
        R"((?:\.[A-Za-z0-9](?:[A-Za-z0-9\-]{0,61}[A-Za-z0-9])?)*>)"));
    const auto anchored = [&](const QRegularExpression& pattern) {
        return pattern
            .match(text, index, QRegularExpression::NormalMatch,
                   QRegularExpression::AnchorAtOffsetMatchOption)
            .hasMatch();
    };
    return anchored(uri) || anchored(email);
}

// Skip explicit links/images, including a URL used as the label. Their destination owns the
// click, and inserting an autolink in the label would create a nested link.
qsizetype markdownLinkEnd(const QString& text, const qsizetype start,
                          const QSet<QString>& references) {
    const auto closing = [&](qsizetype index, const QChar open, const QChar close) {
        int depth = 0;
        for (; index < text.size(); ++index) {
            const QChar c = text.at(index);
            if (c == u'\\') {
                ++index;
            } else if (c == open) {
                ++depth;
            } else if (c == close && --depth == 0) {
                return index + 1;
            }
        }
        return qsizetype(-1);
    };
    const qsizetype labelEnd = closing(start, u'[', u']');
    if (labelEnd < 0)
        return start;
    const QString label = text.mid(start + 1, labelEnd - start - 2).simplified().toCaseFolded();
    const qsizetype shortcutEnd = references.contains(label) ? labelEnd : start;
    if (labelEnd >= text.size())
        return shortcutEnd;
    const QChar next = text.at(labelEnd);
    if (next != u'(' && next != u'[')
        return shortcutEnd;
    const qsizetype end = closing(labelEnd, next, next == u'(' ? u')' : u']');
    return end < 0 ? start : end;
}

qsizetype bareUrlEnd(const QString& text, const qsizetype start) {
    static const QRegularExpression url(
        QStringLiteral(R"((?<![\p{L}\p{N}/@])https?://[^\s<>`"']+)"),
        QRegularExpression::CaseInsensitiveOption);
    const auto match = url.match(text, start, QRegularExpression::NormalMatch,
                                 QRegularExpression::AnchorAtOffsetMatchOption);
    if (!match.hasMatch())
        return start;
    QString candidate = match.captured();
    // An emphasis marker immediately before the URL may close after it. Preserve that markup.
    QString emphasisClosing;
    if (start > 0 && (text.at(start - 1) == u'*' || text.at(start - 1) == u'_')) {
        const QChar marker = text.at(start - 1);
        qsizetype count = 1;
        while (start > count && text.at(start - count - 1) == marker)
            ++count;
        if (marker == u'_' && start > count && text.at(start - count - 1).isLetterOrNumber())
            return start;
        emphasisClosing = QString(count, marker);
    }
    while (!candidate.isEmpty()) {
        if (!emphasisClosing.isEmpty() && candidate.endsWith(emphasisClosing)) {
            candidate.chop(emphasisClosing.size());
            emphasisClosing.clear();
            continue;
        }
        const QChar last = candidate.back();
        if (QStringLiteral(".,;:!?").contains(last) ||
            (last == u')' && candidate.count(u')') > candidate.count(u'(')) ||
            (last == u']' && candidate.count(u']') > candidate.count(u'[')) ||
            (last == u'}' && candidate.count(u'}') > candidate.count(u'{'))) {
            candidate.chop(1);
        } else {
            break;
        }
    }
    const QUrl parsed(candidate);
    return parsed.isValid() && !parsed.host().isEmpty() ? start + candidate.size() : start;
}

// One paragraph of inline Markdown, which may span several lines. A code span cannot cross a
// blank line, so an unmatched backtick run only reaches the end of its paragraph.
QString transformInline(const QString& text, const bool linkify, const QSet<QString>& references) {
    QString output;
    output.reserve(text.size() + 8);
    const qsizetype size = text.size();
    for (qsizetype index = 0; index < size;) {
        const QChar character = text.at(index);
        if (linkify) {
            // Reference definitions are metadata, not prose. Leave destinations/titles intact.
            if (index == 0 || text.at(index - 1) == u'\n') {
                static const QRegularExpression reference(QStringLiteral(R"( {0,3}\[[^\]\n]+\]:)"));
                if (reference
                        .match(text, index, QRegularExpression::NormalMatch,
                               QRegularExpression::AnchorAtOffsetMatchOption)
                        .hasMatch()) {
                    const qsizetype newline = text.indexOf(u'\n', index);
                    const qsizetype end = newline < 0 ? size : newline;
                    output += text.mid(index, end - index);
                    index = end;
                    continue;
                }
            }
            if (character == u'<' && autolinkAt(text, index)) {
                const qsizetype end = text.indexOf(u'>', index) + 1;
                output += text.mid(index, end - index);
                index = end;
                continue;
            }
            if (character == u'[') {
                const qsizetype end = markdownLinkEnd(text, index, references);
                if (end > index) {
                    output += text.mid(index, end - index);
                    index = end;
                    continue;
                }
            }
            if (character.toLower() == u'h') {
                const qsizetype end = bareUrlEnd(text, index);
                if (end > index) {
                    output += u'<' + text.mid(index, end - index) + u'>';
                    index = end;
                    continue;
                }
            }
        }
        if (character == u'\\' && index + 1 < size) {
            output += text.mid(index, 2);
            index += 2;
            continue;
        }
        if (character == u'`') {
            qsizetype run = 1;
            while (index + run < size && text.at(index + run) == u'`')
                ++run;
            qsizetype closing = -1;
            for (qsizetype cursor = index + run; cursor < size;) {
                if (text.at(cursor) != u'`') {
                    ++cursor;
                    continue;
                }
                qsizetype end = cursor;
                while (end < size && text.at(end) == u'`')
                    ++end;
                if (end - cursor == run) {
                    closing = cursor;
                    break;
                }
                cursor = end;
            }
            const qsizetype next = closing < 0 ? index + run : closing + run;
            output += text.mid(index, next - index);
            index = next;
            continue;
        }
        // "](<" opens a pointy-bracket link destination, not HTML.
        const bool linkDestination =
            index >= 2 && text.at(index - 1) == u'(' && text.at(index - 2) == u']';
        if (!linkify && character == u'<' && opensHtml(text, index) && !linkDestination &&
            !autolinkAt(text, index))
            output += u'\\';
        output += character;
        ++index;
    }
    return output;
}

QString transformProse(const QString& markdown, const bool linkify) {
    QSet<QString> references;
    if (linkify) {
        static const QRegularExpression definition(QStringLiteral(R"(^ {0,3}\[([^\]\n]+)\]:)"),
                                                   QRegularExpression::MultilineOption);
        auto matches = definition.globalMatch(markdown);
        while (matches.hasNext())
            references.insert(matches.next().captured(1).simplified().toCaseFolded());
    }
    static const QRegularExpression fenceOpening(QStringLiteral(R"(^\s*(`{3,}|~{3,})(.*)$)"));
    const QStringList lines = markdown.split(u'\n');
    QStringList output;
    QStringList paragraph;
    const auto flush = [&] {
        if (paragraph.isEmpty())
            return;
        output.append(transformInline(paragraph.join(u'\n'), linkify, references));
        paragraph.clear();
    };
    bool previousBlank = true;
    bool inList = false;
    bool inIndentedCode = false;
    qsizetype listContentIndent = 0;
    const auto structuralLine = [linkify](QString line) {
        if (linkify) {
            static const QRegularExpression quote(QStringLiteral(R"(^ {0,3}>[ \t]?)"));
            for (auto match = quote.match(line); match.hasMatch(); match = quote.match(line))
                line.remove(0, match.capturedLength());
        }
        return line;
    };
    for (qsizetype index = 0; index < lines.size(); ++index) {
        const QString& line = lines.at(index);
        const QString content = structuralLine(line);
        const auto fence = fenceOpening.match(content);
        const QString marker = fence.captured(1);
        // A backtick fence's info string cannot contain a backtick: "```a```" is a code span.
        if (fence.hasMatch() && !(marker.front() == u'`' && fence.captured(2).contains(u'`'))) {
            flush();
            qsizetype end = index + 1;
            for (; end < lines.size(); ++end) {
                const QString closing = structuralLine(lines.at(end)).trimmed();
                if (closing.size() >= marker.size() &&
                    closing == QString(closing.size(), marker.front()))
                    break;
            }
            // A fence still open at the end (a streaming answer) is code to the end.
            end = qMin(end, lines.size() - 1);
            output.append(lines.mid(index, end - index + 1));
            index = end;
            previousBlank = false;
            inIndentedCode = false;
            continue;
        }
        if (content.trimmed().isEmpty()) {
            flush();
            output.append(line);
            previousBlank = true;
            continue;
        }
        const qsizetype indent = indentation(content);
        // Four columns of indentation start a code block only after a blank line and outside a
        // list, where they would continue a list item instead.
        const bool listCode = linkify && inList && indent >= listContentIndent + 4;
        if (indent >= 4 && (inIndentedCode || (previousBlank && (!inList || listCode)))) {
            flush();
            output.append(line);
            inIndentedCode = true;
            previousBlank = false;
            continue;
        }
        inIndentedCode = false;
        if (listItem(content)) {
            inList = true;
            static const QRegularExpression listPrefix(
                QStringLiteral(R"(^\s*(?:[-+*]|\d{1,9}[.)])[ \t]+)"));
            listContentIndent = listPrefix.match(content).capturedLength();
        } else if (indent == 0 && previousBlank)
            inList = false;
        paragraph.append(line);
        previousBlank = false;
    }
    flush();
    return output.join(u'\n');
}

} // namespace

QString escapeRawHtml(const QString& markdown) {
    return markdown.contains(u'<') ? transformProse(markdown, false) : markdown;
}

QString prepareForDisplay(const QString& markdown) {
    const QString escaped = escapeRawHtml(markdown);
    return escaped.contains(QStringLiteral("http"), Qt::CaseInsensitive)
               ? transformProse(escaped, true)
               : escaped;
}

} // namespace ketplus::chat_markdown

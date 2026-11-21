#include "ThemeInput.h"

#include <QJsonParseError>

namespace ket::theme::io {
namespace {
void location(Error* error, const QByteArray& data, qsizetype offset, const QString& message) {
    if (!error)
        return;
    offset = qBound(qsizetype(0), offset, data.size());
    qsizetype lineStart = 0;
    error->message = message;
    error->line = 1;
    for (qsizetype i = 0; i < offset; ++i) {
        if (data[i] == '\r') {
            if (i + 1 < offset && data[i + 1] == '\n')
                ++i;
            ++error->line;
            lineStart = i + 1;
        } else if (data[i] == '\n') {
            ++error->line;
            lineStart = i + 1;
        }
    }
    error->column = QString::fromUtf8(data.mid(lineStart, offset - lineStart)).size() + 1;
}
} // namespace
QJsonDocument parseJsonc(const QByteArray& data, Error* error) {
    if (error)
        *error = {};
    if (data.size() > MaxFileBytes) {
        location(error, data, 0, QStringLiteral("JSONC exceeds 5 MiB file limit"));
        return {};
    }
    QByteArray json = data;
    bool string = false;
    for (qsizetype i = 0; i < data.size(); ++i) {
        const char c = data[i];
        if (string) {
            if (c == '\\')
                ++i;
            else if (c == '"')
                string = false;
            continue;
        }
        if (c == '"') {
            string = true;
            continue;
        }
        if (c != '/' || i + 1 >= data.size())
            continue;
        const char next = data[i + 1];
        if (next != '/' && next != '*')
            continue;
        const auto start = i;
        json[i++] = ' ';
        json[i] = ' ';
        bool closed = next == '/';
        while (++i < data.size()) {
            if (next == '/' && (data[i] == '\n' || data[i] == '\r'))
                break;
            if (next == '*' && data[i] == '*' && i + 1 < data.size() && data[i + 1] == '/') {
                json[i] = ' ';
                json[++i] = ' ';
                closed = true;
                break;
            }
            if (data[i] != '\n' && data[i] != '\r')
                json[i] = ' ';
        }
        if (!closed) {
            location(error, data, start, QStringLiteral("Unterminated block comment"));
            return {};
        }
    }
    // Second lexical pass only removes a comma after a value and before a closing
    // bracket. Comments have become whitespace; byte offsets and line breaks survive.
    string = false;
    char previous = 0;
    for (qsizetype i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (string) {
            if (c == '\\')
                ++i;
            else if (c == '"') {
                string = false;
                previous = '"';
            }
            continue;
        }
        if (c == '"') {
            string = true;
            continue;
        }
        if (c == ',') {
            auto j = i + 1;
            while (j < json.size() &&
                   (json[j] == ' ' || json[j] == '\t' || json[j] == '\r' || json[j] == '\n'))
                ++j;
            if (j < json.size() && (json[j] == ']' || json[j] == '}') && previous &&
                previous != '[' && previous != '{' && previous != ':' && previous != ',')
                json[i] = ' ';
        }
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
            previous = c;
    }
    QJsonParseError parseError;
    auto document = QJsonDocument::fromJson(json, &parseError);
    if (parseError.error != QJsonParseError::NoError)
        location(error, data, parseError.offset, parseError.errorString());
    return document;
}
} // namespace ket::theme::io

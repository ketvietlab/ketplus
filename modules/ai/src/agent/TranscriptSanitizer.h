#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

// QtCore-only redaction shared by persisted retention excerpts and transcript presentation.
#ifndef Q_MOC_RUN
namespace ketplus {
class TranscriptSanitizer final {
  public:
    [[nodiscard]] static bool isSensitiveKey(const QString& key);
    [[nodiscard]] static QString text(QString value);
    [[nodiscard]] static QString url(const QString& value);
    [[nodiscard]] static QJsonValue json(const QJsonValue& value, const QString& key = {});
};

inline bool TranscriptSanitizer::isSensitiveKey(const QString& key) {
    static const QSet<QString> exact{QStringLiteral("authorization"),
                                     QStringLiteral("cookie"),
                                     QStringLiteral("credentials"),
                                     QStringLiteral("password"),
                                     QStringLiteral("passwd"),
                                     QStringLiteral("secret"),
                                     QStringLiteral("token"),
                                     QStringLiteral("access_token"),
                                     QStringLiteral("refresh_token"),
                                     QStringLiteral("api_key"),
                                     QStringLiteral("apikey"),
                                     QStringLiteral("client_secret"),
                                     QStringLiteral("auth"),
                                     QStringLiteral("code"),
                                     QStringLiteral("key")};
    const QString normalized = QString(key).toLower().replace('-', '_');
    return exact.contains(normalized) || normalized.endsWith(QStringLiteral("_token")) ||
           normalized.endsWith(QStringLiteral("_password")) ||
           normalized.endsWith(QStringLiteral("_secret")) ||
           normalized.endsWith(QStringLiteral("_api_key"));
}

inline QString TranscriptSanitizer::url(const QString& value) {
    QUrl parsed(value);
    if (!parsed.isValid() || parsed.scheme().isEmpty())
        return value;
    if (!parsed.userName().isEmpty() || !parsed.password().isEmpty()) {
        parsed.setUserName(QStringLiteral("redacted"));
        parsed.setPassword(QStringLiteral("redacted"));
    }
    QUrlQuery query(parsed);
    const auto items = query.queryItems(QUrl::FullyDecoded);
    query.clear();
    for (const auto& item : items) {
        query.addQueryItem(item.first,
                           isSensitiveKey(item.first) ? QStringLiteral("[redacted]") : item.second);
    }
    parsed.setQuery(query);
    if (parsed.hasFragment() && parsed.fragment().contains(QLatin1Char('='))) {
        QUrlQuery fragmentQuery(parsed.fragment(QUrl::FullyDecoded));
        const auto fragmentItems = fragmentQuery.queryItems(QUrl::FullyDecoded);
        fragmentQuery.clear();
        for (const auto& item : fragmentItems) {
            fragmentQuery.addQueryItem(item.first, isSensitiveKey(item.first)
                                                       ? QStringLiteral("[redacted]")
                                                       : item.second);
        }
        parsed.setFragment(fragmentQuery.toString(QUrl::FullyDecoded));
    }
    return parsed.toString(QUrl::FullyEncoded);
}

inline QString TranscriptSanitizer::text(QString value) {
    // Every string of every restored item comes through here, command output
    // included, so each pattern first needs the text it cannot match without.
    // Compiling "\\s" per call and parsing whole outputs as URLs once made
    // building a history page cost tens of milliseconds.
    static const QRegularExpression whitespace(QStringLiteral("\\s"));
    if (value.contains(QLatin1Char(':')) && !value.contains(whitespace)) {
        const QUrl possibleUrl(value);
        if (possibleUrl.isValid() && !possibleUrl.scheme().isEmpty())
            return url(value);
    }
    static const QRegularExpression assignment(
        QStringLiteral(R"((?i)\b(token|password|secret|api[_-]?key|authorization)=([^\s]+))"));
    static const QRegularExpression option(QStringLiteral(
        R"((?i)(--(?:token|password|secret|api[_-]?key|authorization)\s+)([^\s]+))"));
    static const QRegularExpression bearer(
        QStringLiteral(R"((?i)(\bBearer\s+)([A-Za-z0-9._~+\-/=]+))"));
    if (value.contains(QLatin1Char('=')))
        value.replace(assignment, QStringLiteral("\\1=[redacted]"));
    if (value.contains(QLatin1StringView("--")))
        value.replace(option, QStringLiteral("\\1[redacted]"));
    if (value.contains(QLatin1StringView("bearer"), Qt::CaseInsensitive))
        value.replace(bearer, QStringLiteral("\\1[redacted]"));
    if (!value.contains(QLatin1StringView("://")))
        return value;
    // A label such as "Ran: curl ..." is prose, not a URL with a "ran" scheme.
    // Sanitize embedded URLs separately without percent-encoding the whole label.
    // The scheme starts at the first letter of the scheme-character run before "://". Matching
    // starts only at such a run and never backtracks into it, so long output without "://"
    // stays linear when PCRE2 runs without JIT.
    static const QRegularExpression embeddedUrl(
        QStringLiteral(R"((?<![A-Za-z0-9+.-])[0-9+.-]*+([A-Za-z][A-Za-z0-9+.-]*+://[^\s"'<>]++))"));
    // Matches share the input QString. Mutating it per URL detaches/copies the entire
    // remaining text each time; build one output while traversing the immutable input.
    auto matches = embeddedUrl.globalMatch(value);
    if (!matches.hasNext())
        return value;
    QString result;
    result.reserve(value.size());
    qsizetype offset = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        result.append(QStringView(value).mid(offset, match.capturedStart(1) - offset));
        result.append(url(match.captured(1)));
        offset = match.capturedEnd(1);
    }
    result.append(QStringView(value).mid(offset));
    return result;
}

inline QJsonValue TranscriptSanitizer::json(const QJsonValue& value, const QString& key) {
    if (isSensitiveKey(key))
        return QStringLiteral("[redacted]");
    if (value.isObject()) {
        QJsonObject result;
        const auto object = value.toObject();
        // MCP image/audio content blocks carry base64 payloads; model rows keep only a marker.
        // Images are materialized separately (McpToolResultMedia) with size limits.
        const QString blockType = object.value(QStringLiteral("type")).toString();
        const bool inlineMedia =
            (blockType == QStringLiteral("image") || blockType == QStringLiteral("audio")) &&
            object.value(QStringLiteral("data")).isString();
        for (auto it = object.begin(); it != object.end(); ++it) {
            if (inlineMedia && it.key() == QStringLiteral("data"))
                result.insert(it.key(), QStringLiteral("[inline %1 omitted]").arg(blockType));
            else if (it.key() == QStringLiteral("blob") && it.value().isString())
                // Embedded MCP resource contents ({resource:{uri, mimeType, blob}}).
                result.insert(it.key(), QStringLiteral("[inline resource omitted]"));
            else
                result.insert(it.key(), json(it.value(), it.key()));
        }
        return result;
    }
    if (value.isArray()) {
        QJsonArray result;
        for (const auto& item : value.toArray())
            result.append(json(item));
        return result;
    }
    if (value.isString())
        return text(value.toString());
    return value;
}

} // namespace ketplus
#endif

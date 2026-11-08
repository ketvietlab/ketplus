#include "providers/ProviderLineReader.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <optional>
#include <utility>

namespace ketplus::daemon {
namespace {

// Ids, methods and thread ids are short; longer strings are never reported.
constexpr qsizetype maximumFieldBytes = 1024;
// Deeper nesting than this is not scanned further; requested fields are shallow.
constexpr qsizetype maximumScanDepth = 64;

std::optional<QString> decodedString(const QByteArray& token) {
    if (token.size() > maximumFieldBytes)
        return std::nullopt;
    // Qt accepts some non-JSON escapes and unescaped controls. Reject those before
    // using its Unicode decoder so malformed metadata cannot become a different id.
    for (qsizetype i = 0; i < token.size(); ++i) {
        if (static_cast<unsigned char>(token.at(i)) < 0x20)
            return std::nullopt;
        if (token.at(i) != '\\')
            continue;
        if (++i >= token.size())
            return std::nullopt;
        switch (token.at(i)) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 'r':
        case 't':
            break;
        case 'u':
            if (token.size() - i <= 4)
                return std::nullopt;
            for (qsizetype end = i + 4; i < end;) {
                const char hex = token.at(++i);
                if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') ||
                      (hex >= 'A' && hex <= 'F')))
                    return std::nullopt;
            }
            break;
        default:
            return std::nullopt;
        }
    }
    const QJsonDocument document = QJsonDocument::fromJson("[\"" + token + "\"]");
    if (!document.isArray() || document.array().size() != 1 || !document.array().first().isString())
        return std::nullopt;
    return document.array().first().toString();
}

} // namespace

ProviderLineReader::ProviderLineReader(const qsizetype maximumLineBytes, QStringList fieldPaths)
    : maximumLineBytes_(maximumLineBytes), fieldPaths_(std::move(fieldPaths)) {}

void ProviderLineReader::clear() {
    buffer_.clear();
    resetScan();
    ++generation_;
}

void ProviderLineReader::resetScan() {
    discarding_ = false;
    routingValid_ = true;
    discarded_ = {};
    frames_.clear();
    skippedDepth_ = 0;
    inString_ = escaped_ = stringIsKey_ = capturing_ = false;
    token_.clear();
    scalar_.clear();
}

void ProviderLineReader::append(const QByteArray& data, const LineHandler& line,
                                const OversizedHandler& oversized) {
    const quint64 generation = generation_;
    const auto report = [&] {
        const OversizedProviderLine done = std::exchange(discarded_, {});
        resetScan();
        oversized(done);
    };
    qsizetype offset = 0;
    while (offset < data.size() && generation == generation_) {
        const qsizetype newline = data.indexOf('\n', offset);
        const qsizetype end = newline < 0 ? data.size() : newline;
        if (discarding_) {
            const QByteArray chunk = data.mid(offset, end - offset);
            discarded_.bytes += chunk.size();
            scan(chunk);
            if (newline < 0)
                return;
            offset = newline + 1;
            report();
            continue;
        }
        buffer_.append(data.mid(offset, end - offset));
        offset = end + 1;
        if (buffer_.size() > maximumLineBytes_) {
            discarding_ = true;
            discarded_.bytes = buffer_.size();
            scan(std::exchange(buffer_, {}));
            if (newline >= 0)
                report();
            continue;
        }
        if (newline < 0)
            return;
        const QByteArray complete = std::exchange(buffer_, {}).trimmed();
        if (!complete.isEmpty())
            line(complete);
    }
}

void ProviderLineReader::scan(const QByteArray& data) {
    for (const char c : data) {
        if (inString_) {
            if (escaped_) {
                escaped_ = false;
            } else if (c == '\\') {
                escaped_ = true;
                if (capturing_ && token_.size() <= maximumFieldBytes)
                    token_.append(c);
                continue;
            } else if (c == '"') {
                inString_ = false;
                const auto value = capturing_ ? decodedString(token_) : std::nullopt;
                // An unknown overlong key cannot name a requested path. Other invalid
                // captured strings invalidate routing for this line, not just one field.
                if (capturing_ && !value && (!stringIsKey_ || token_.size() <= maximumFieldBytes)) {
                    routingValid_ = false;
                    discarded_.fields.clear();
                }
                if (stringIsKey_ && !frames_.isEmpty())
                    frames_.last().key = value.value_or(QString());
                else if (value)
                    finishValue(*value);
                token_.clear();
                capturing_ = false;
                continue;
            }
            if (capturing_ && token_.size() <= maximumFieldBytes)
                token_.append(c);
            continue;
        }
        switch (c) {
        case '"':
            finishScalar();
            inString_ = true;
            stringIsKey_ = skippedDepth_ == 0 && !frames_.isEmpty() && frames_.last().object &&
                           frames_.last().expectKey;
            capturing_ = stringIsKey_ || fieldPaths_.contains(valuePath());
            break;
        case '{':
        case '[':
            finishScalar();
            // Keep only the shallow routing stack. Count deeper containers without
            // allocating frames, then resume the outer object after they close.
            if (skippedDepth_ > 0 || frames_.size() >= maximumScanDepth)
                ++skippedDepth_;
            else
                frames_.append(Frame{c == '{', c == '{', {}});
            break;
        case '}':
        case ']':
            finishScalar();
            if (skippedDepth_ > 0)
                --skippedDepth_;
            else if (!frames_.isEmpty())
                frames_.removeLast();
            break;
        case ':':
            finishScalar();
            if (skippedDepth_ == 0 && !frames_.isEmpty())
                frames_.last().expectKey = false;
            break;
        case ',':
            finishScalar();
            if (skippedDepth_ == 0 && !frames_.isEmpty() && frames_.last().object)
                frames_.last().expectKey = true;
            break;
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            finishScalar();
            break;
        default:
            if (skippedDepth_ == 0 && scalar_.size() <= maximumFieldBytes)
                scalar_.append(c);
            break;
        }
    }
}

void ProviderLineReader::finishScalar() {
    if (scalar_.isEmpty())
        return;
    const QByteArray text = std::exchange(scalar_, {});
    if (!fieldPaths_.contains(valuePath()))
        return;
    const QJsonDocument document = QJsonDocument::fromJson('[' + text + ']');
    if (document.isArray() && document.array().size() == 1)
        finishValue(document.array().first());
}

void ProviderLineReader::finishValue(const QJsonValue& value) {
    const QString path = valuePath();
    if (routingValid_ && !discarded_.fields.contains(path))
        discarded_.fields.insert(path, value);
}

QString ProviderLineReader::valuePath() const {
    if (skippedDepth_ > 0)
        return QStringLiteral("[]");
    QStringList keys;
    for (const Frame& frame : frames_) {
        // A value inside an array, or a key still being read, never matches a field path.
        if (!frame.object || frame.expectKey || frame.key.contains(QLatin1Char('.')))
            return QStringLiteral("[]");
        keys.append(frame.key);
    }
    return keys.join(QLatin1Char('.'));
}

} // namespace ketplus::daemon

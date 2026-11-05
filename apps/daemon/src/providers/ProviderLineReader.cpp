#include "providers/ProviderLineReader.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <utility>

namespace ketplus::daemon {
namespace {

// Ids, methods and thread ids are short; longer strings are never reported.
constexpr qsizetype maximumFieldBytes = 1024;
// Deeper nesting than this is not scanned further; requested fields are shallow.
constexpr qsizetype maximumScanDepth = 64;

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
    discarded_ = {};
    frames_.clear();
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
                continue;
            } else if (c == '"') {
                inString_ = false;
                if (stringIsKey_ && !frames_.isEmpty())
                    frames_.last().key = QString::fromUtf8(token_);
                else if (capturing_ && token_.size() <= maximumFieldBytes)
                    finishValue(QString::fromUtf8(token_));
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
            stringIsKey_ = !frames_.isEmpty() && frames_.last().object && frames_.last().expectKey;
            capturing_ = stringIsKey_ || fieldPaths_.contains(valuePath());
            break;
        case '{':
        case '[':
            finishScalar();
            // Below the scan depth a container is treated as an array: nothing in it matches.
            frames_.append(frames_.size() < maximumScanDepth ? Frame{c == '{', c == '{', {}}
                                                             : Frame{});
            break;
        case '}':
        case ']':
            finishScalar();
            if (!frames_.isEmpty())
                frames_.removeLast();
            break;
        case ':':
            finishScalar();
            if (!frames_.isEmpty())
                frames_.last().expectKey = false;
            break;
        case ',':
            finishScalar();
            if (!frames_.isEmpty() && frames_.last().object)
                frames_.last().expectKey = true;
            break;
        case ' ':
        case '\t':
        case '\r':
        case '\n':
            finishScalar();
            break;
        default:
            if (scalar_.size() <= maximumFieldBytes)
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
    if (!discarded_.fields.contains(path))
        discarded_.fields.insert(path, value);
}

QString ProviderLineReader::valuePath() const {
    QStringList keys;
    for (const Frame& frame : frames_) {
        // A value inside an array, or a key still being read, never matches a field path.
        if (!frame.object || frame.expectKey)
            return QStringLiteral("[]");
        keys.append(frame.key);
    }
    return keys.join(QLatin1Char('.'));
}

} // namespace ketplus::daemon

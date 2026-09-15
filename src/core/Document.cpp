#include "core/Document.h"

#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStringDecoder>
#include <QStringEncoder>

#include <utility>

namespace ketplus {
namespace {

const QByteArray utf8Bom("\xEF\xBB\xBF", 3);
const QByteArray utf16LEBom("\xFF\xFE", 2);
const QByteArray utf16BEBom("\xFE\xFF", 2);

QByteArray withoutPrefix(const QByteArray& raw, const QByteArray& prefix) {
    return raw.startsWith(prefix) ? raw.mid(prefix.size()) : raw;
}

} // namespace

QString textEncodingName(const TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8:
        return QStringLiteral("UTF-8");
    case TextEncoding::Utf8Bom:
        return QStringLiteral("UTF-8 with BOM");
    case TextEncoding::Utf16LE:
        return QStringLiteral("UTF-16 LE");
    case TextEncoding::Utf16BE:
        return QStringLiteral("UTF-16 BE");
    case TextEncoding::Latin1:
        return QStringLiteral("Western (ISO-8859-1)");
    }
    return {};
}

QList<TextEncoding> supportedTextEncodings() {
    return {TextEncoding::Utf8, TextEncoding::Utf8Bom, TextEncoding::Utf16LE,
            TextEncoding::Utf16BE, TextEncoding::Latin1};
}

QString lineEndingName(const LineEnding lineEnding) {
    switch (lineEnding) {
    case LineEnding::Lf:
        return QStringLiteral("LF");
    case LineEnding::CrLf:
        return QStringLiteral("CRLF");
    case LineEnding::Cr:
        return QStringLiteral("CR");
    }
    return {};
}

Document::LoadResult Document::load(const QString& filePath) { return loadWith(filePath, {}); }

Document::LoadResult Document::load(const QString& filePath, const TextEncoding encoding) {
    return loadWith(filePath, encoding);
}

Document::LoadResult Document::loadWith(const QString& filePath,
                                        const std::optional<TextEncoding> encoding) {
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {false, file.errorString(), {}};
    }

    const QByteArray raw = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        return {false, file.errorString(), {}};
    }

    filePath_ = QFileInfo(filePath).absoluteFilePath();
    encoding_ = encoding.value_or(detectEncoding(raw));
    modified_ = false;
    return {true, {}, decodeToUtf8(raw, encoding_)};
}

Document::Result Document::save(const QByteArray& content) {
    if (isUntitled()) {
        return {false, QStringLiteral("Document has no file path")};
    }
    return saveAs(filePath_, content);
}

Document::Result Document::saveAs(const QString& filePath, const QByteArray& content) {
    const auto encoded = encodeFromUtf8(content, encoding_);
    if (!encoded.has_value()) {
        return {false, QStringLiteral("Some characters cannot be saved as %1.")
                           .arg(textEncodingName(encoding_))};
    }

    QSaveFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        return {false, file.errorString()};
    }

    if (file.write(*encoded) != encoded->size()) {
        return {false, file.errorString()};
    }
    if (!file.commit()) {
        return {false, file.errorString()};
    }

    filePath_ = QFileInfo(filePath).absoluteFilePath();
    modified_ = false;
    return {true, {}};
}

const QString& Document::filePath() const noexcept {
    return filePath_;
}

bool Document::isUntitled() const noexcept {
    return filePath_.isEmpty();
}

bool Document::isModified() const noexcept {
    return modified_;
}

QString Document::displayName() const {
    return isUntitled() ? QStringLiteral("Untitled") : QFileInfo(filePath_).fileName();
}

TextEncoding Document::textEncoding() const noexcept { return encoding_; }

void Document::setModified(const bool modified) noexcept {
    modified_ = modified;
}

void Document::setTextEncoding(const TextEncoding encoding) noexcept { encoding_ = encoding; }

TextEncoding Document::detectEncoding(const QByteArray& raw) {
    if (raw.startsWith(utf8Bom)) {
        return TextEncoding::Utf8Bom;
    }
    if (raw.startsWith(utf16LEBom)) {
        return TextEncoding::Utf16LE;
    }
    if (raw.startsWith(utf16BEBom)) {
        return TextEncoding::Utf16BE;
    }
    // Stateless, so a truncated sequence at the end of the file counts as invalid.
    QStringDecoder decoder(QStringConverter::Utf8, QStringConverter::Flag::Stateless);
    [[maybe_unused]] const QString decoded = decoder.decode(raw);
    // Bytes that are not valid UTF-8 are most likely a legacy single-byte encoding.
    return decoder.hasError() ? TextEncoding::Latin1 : TextEncoding::Utf8;
}

QByteArray Document::decodeToUtf8(const QByteArray& raw, const TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8:
        return raw;
    case TextEncoding::Utf8Bom:
        return withoutPrefix(raw, utf8Bom);
    case TextEncoding::Utf16LE: {
        QStringDecoder decoder(QStringConverter::Utf16LE);
        return QString(decoder.decode(withoutPrefix(raw, utf16LEBom))).toUtf8();
    }
    case TextEncoding::Utf16BE: {
        QStringDecoder decoder(QStringConverter::Utf16BE);
        return QString(decoder.decode(withoutPrefix(raw, utf16BEBom))).toUtf8();
    }
    case TextEncoding::Latin1:
        return QString::fromLatin1(raw).toUtf8();
    }
    return raw;
}

std::optional<QByteArray> Document::encodeFromUtf8(const QByteArray& utf8,
                                                   const TextEncoding encoding) {
    switch (encoding) {
    case TextEncoding::Utf8:
        return utf8;
    case TextEncoding::Utf8Bom:
        return utf8Bom + utf8;
    case TextEncoding::Utf16LE: {
        QStringEncoder encoder(QStringConverter::Utf16LE);
        return utf16LEBom + QByteArray(encoder.encode(QString::fromUtf8(utf8)));
    }
    case TextEncoding::Utf16BE: {
        QStringEncoder encoder(QStringConverter::Utf16BE);
        return utf16BEBom + QByteArray(encoder.encode(QString::fromUtf8(utf8)));
    }
    case TextEncoding::Latin1: {
        const QString text = QString::fromUtf8(utf8);
        for (const QChar character : text) {
            if (character.unicode() > 0xFF) {
                return std::nullopt;
            }
        }
        return text.toLatin1();
    }
    }
    return utf8;
}

} // namespace ketplus

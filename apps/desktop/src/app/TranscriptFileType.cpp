#include "app/TranscriptFileType.h"

#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QMimeType>
#include <QStringDecoder>

namespace ketplus {
namespace {

bool isNonTextFormat(const QMimeType& type) {
    // SVG and other text-encoded media still belong in their platform viewer.
    const QString name = type.name();
    if (name.startsWith("image/") || name.startsWith("audio/") || name.startsWith("video/"))
        return true;
    return type.isValid() && !type.isDefault() && name != "application/x-zerosize" &&
           !type.inherits("text/plain");
}

} // namespace

bool isTranscriptTextFile(const QString& path) {
    if (!QFileInfo(path).isFile())
        return false;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    // Bounded sniffing keeps activation independent of the full document size.
    const QByteArray sample = file.read(8192);
    if (file.error() != QFileDevice::NoError)
        return false;
    const QMimeDatabase database;
    if (isNonTextFormat(database.mimeTypeForFile(path, QMimeDatabase::MatchExtension)))
        return false;
    if (sample.isEmpty())
        return true;

    // BOM-marked Unicode is text even though its byte representation contains NULs.
    const auto encoding = QStringConverter::encodingForData(sample);
    if (encoding && *encoding != QStringConverter::Utf8) {
        QStringDecoder decoder(*encoding);
        const QString text = decoder(sample);
        return !decoder.hasError() && !text.contains(QChar::Null);
    }
    if (isNonTextFormat(database.mimeTypeForData(sample)))
        return false;
    for (const unsigned char byte : sample) {
        if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r' && byte != '\f')
            return false;
    }
    return true;
}

} // namespace ketplus

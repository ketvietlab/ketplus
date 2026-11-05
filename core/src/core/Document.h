#pragma once

#include <QByteArray>
#include <QList>
#include <QString>

#include <optional>

namespace ketplus {

enum class TextEncoding { Utf8, Utf8Bom, Utf16LE, Utf16BE, Latin1 };
enum class LineEnding { Lf, CrLf, Cr };

[[nodiscard]] QString textEncodingName(TextEncoding encoding);
[[nodiscard]] QList<TextEncoding> supportedTextEncodings();
[[nodiscard]] QString lineEndingName(LineEnding lineEnding);

class Document final {
public:
    struct Result {
        bool ok{false};
        QString error;
    };

    struct LoadResult {
        bool ok{false};
        QString error;
        // Always UTF-8, whatever the file's encoding on disk.
        QByteArray content;
    };

    // Files above this are refused rather than read whole on the UI thread.
    static constexpr qint64 maximumFileBytes = 256LL * 1024 * 1024;

    // Loads and detects the encoding from a byte order mark or UTF-8 validity.
    [[nodiscard]] LoadResult load(const QString& filePath);
    // Loads and decodes with the given encoding instead of detecting it.
    [[nodiscard]] LoadResult load(const QString& filePath, TextEncoding encoding);
    // Loads the same path again, detecting the encoding unless the user picked one.
    [[nodiscard]] LoadResult reload(const QString& filePath);
    // `content` is UTF-8 and is written in the document's text encoding.
    [[nodiscard]] Result save(const QByteArray& content);
    [[nodiscard]] Result saveAs(const QString& filePath, const QByteArray& content);

    [[nodiscard]] const QString& filePath() const noexcept;
    [[nodiscard]] bool isUntitled() const noexcept;
    [[nodiscard]] bool isModified() const noexcept;
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] TextEncoding textEncoding() const noexcept;
    // Whether the file differs from what was last loaded or saved: deleted, or new bytes.
    // False for untitled documents.
    [[nodiscard]] bool changedOnDisk() const;

    void setModified(bool modified) noexcept;
    // A user's choice, kept across reloads.
    void setTextEncoding(TextEncoding encoding) noexcept;

    [[nodiscard]] static TextEncoding detectEncoding(const QByteArray& raw);
    [[nodiscard]] static QByteArray decodeToUtf8(const QByteArray& raw, TextEncoding encoding);
    // Returns nothing when the text has characters the encoding cannot represent.
    [[nodiscard]] static std::optional<QByteArray> encodeFromUtf8(const QByteArray& utf8,
                                                                  TextEncoding encoding);

private:
    [[nodiscard]] LoadResult loadWith(const QString& filePath, std::optional<TextEncoding> encoding);

    void rememberDiskState(const QByteArray& raw);

    QString filePath_;
    TextEncoding encoding_{TextEncoding::Utf8};
    bool encodingExplicit_{false};
    bool modified_{false};
    // What the file held when it was last loaded or saved.
    qint64 diskSize_{-1};
    QByteArray diskDigest_;
};

} // namespace ketplus

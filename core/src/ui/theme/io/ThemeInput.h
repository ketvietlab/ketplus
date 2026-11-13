#pragma once

#include <QByteArray>
#include <QJsonDocument>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>
#include <memory>

namespace ket::theme::io {
inline constexpr qint64 MaxArchiveBytes = 25 * 1024 * 1024;
inline constexpr quint64 MaxExpandedBytes = 100 * 1024 * 1024;
inline constexpr qint64 MaxFileBytes = 5 * 1024 * 1024;
inline constexpr int MaxEntries = 2000;
inline constexpr int MaxIncludeLevels = 16;

struct Error {
    QString message;
    QString source;
    qint64 line = 0;
    qint64 column = 0;
};
// Errors are cleared on entry. A null document means failure.
QJsonDocument parseJsonc(const QByteArray& data, Error* error = nullptr);

struct XmlNode {
    QString name;
    QMap<QString, QString> attributes;
    QString text;
    QVector<XmlNode> children;
};
// Accepts inert external DOCTYPE identifiers, rejects internal subsets/entities.
// No entity resolver or network access is installed.
bool parseXml(const QByteArray& data, XmlNode* root, Error* error = nullptr);

class ZipArchive {
  public:
    ZipArchive();
    ~ZipArchive();
    ZipArchive(const ZipArchive&) = delete;
    ZipArchive& operator=(const ZipArchive&) = delete;
    bool open(const QString& path, Error* error = nullptr);
    QStringList files() const;
    bool read(const QString& path, QByteArray* data, Error* error = nullptr) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

struct JsonSource {
    QString path;
    QJsonDocument document;
};
// Exactly one source: a caller-selected directory or an already validated archive.
// Returned documents are ordered parent first. Merging remains the adapter's job.
class IncludeResolver {
  public:
    explicit IncludeResolver(const QString& rootDirectory);
    explicit IncludeResolver(const ZipArchive& archive);
    bool resolve(const QString& declaringPath, const QString& reference, QString* path,
                 Error* error = nullptr) const;
    bool read(const QString& path, QByteArray* data, Error* error = nullptr) const;
    // Optional shared remaining-byte budget is consumed across calls, including
    // unsuccessful reads/parses, so package variants cannot multiply read work.
    bool loadJsonChain(const QString& path, QVector<JsonSource>* chain, Error* error = nullptr,
                       quint64* remainingBytes = nullptr) const;

  private:
    QString root;
    const ZipArchive* archive = nullptr;
};
} // namespace ket::theme::io

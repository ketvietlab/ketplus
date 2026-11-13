#include "ThemeInput.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <miniz.h>

namespace ket::theme::io {
namespace {
bool fail(Error* error, const QString& message, const QString& source = {}) {
    if (error)
        *error = {message, source};
    return false;
}
bool safeName(const QString& name) {
    if (name.isEmpty() || name.startsWith('/') || name.contains('\\') || name.contains(':') ||
        name.contains(QChar::Null))
        return false;
    for (const auto& part : name.split('/'))
        if (part == QStringLiteral(".."))
            return false;
    return true;
}
} // namespace
struct ZipArchive::Impl {
    QFile file;
    mz_zip_archive zip{};
    QMap<QString, mz_uint> entries;
    bool initialized = false;
    ~Impl() {
        if (initialized)
            mz_zip_reader_end(&zip);
    }
    static size_t read(void* opaque, mz_uint64 offset, void* buffer, size_t count) {
        auto* self = static_cast<Impl*>(opaque);
        if (offset > quint64(self->file.size()) || !self->file.seek(qint64(offset)))
            return 0;
        const auto n = self->file.read(static_cast<char*>(buffer), qint64(count));
        return n < 0 ? 0 : size_t(n);
    }
};
ZipArchive::ZipArchive() : d(std::make_unique<Impl>()) {}
ZipArchive::~ZipArchive() = default;
bool ZipArchive::open(const QString& path, Error* error) {
    if (error)
        *error = {};
    d = std::make_unique<Impl>();
    auto candidate = std::make_unique<Impl>();
    candidate->file.setFileName(path);
    if (!QFileInfo(path).isFile())
        return fail(error, QStringLiteral("ZIP input must be a regular file"), path);
    if (!candidate->file.open(QIODevice::ReadOnly))
        return fail(error, candidate->file.errorString(), path);
    const auto archiveSize = candidate->file.size();
    if (archiveSize < 0 || archiveSize > MaxArchiveBytes)
        return fail(error, QStringLiteral("ZIP exceeds 25 MiB compressed limit"), path);
    candidate->zip.m_pRead = Impl::read;
    candidate->zip.m_pIO_opaque = candidate.get();
    if (!mz_zip_reader_init(&candidate->zip, mz_uint64(archiveSize), 0))
        return fail(error, QStringLiteral("Invalid ZIP directory"), path);
    candidate->initialized = true;
    const auto count = mz_zip_reader_get_num_files(&candidate->zip);
    if (count > MaxEntries)
        return fail(error, QStringLiteral("ZIP exceeds 2,000 entry limit"), path);
    quint64 expanded = 0;
    QSet<QString> names;
    QSet<QString> regularFiles;
    QStringList allNames;
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&candidate->zip, i, &stat))
            return fail(error, QStringLiteral("Invalid ZIP entry"), path);
        const auto size = mz_zip_reader_get_filename(&candidate->zip, i, nullptr, 0);
        if (!size || size > 65536)
            return fail(error, QStringLiteral("Invalid ZIP filename"), path);
        QByteArray raw(qsizetype(size), '\0');
        if (mz_zip_reader_get_filename(&candidate->zip, i, raw.data(), size) != size)
            return fail(error, QStringLiteral("Invalid ZIP filename"), path);
        raw.chop(1);
        const auto name = QString::fromUtf8(raw);
        if (name.toUtf8() != raw || !safeName(name))
            return fail(error, QStringLiteral("Unsafe ZIP path"), name);
        const auto normalized = QDir::cleanPath(name).normalized(QString::NormalizationForm_C);
        const auto key = normalized.toCaseFolded();
        if (normalized == QStringLiteral(".") || names.contains(key))
            return fail(error, QStringLiteral("Duplicate or colliding ZIP path"), name);
        names.insert(key);
        allNames.append(key);
        // UNIX file type is in the high 16 bits. No links or device entries are data.
        const auto type = (stat.m_external_attr >> 16U) & 0170000U;
        if (type && type != 0100000U && type != 0040000U)
            return fail(error, QStringLiteral("ZIP links and special files are forbidden"), name);
        const bool directory = stat.m_is_directory || type == 0040000U;
        if (stat.m_is_encrypted || !stat.m_is_supported)
            return fail(error, QStringLiteral("Encrypted or unsupported ZIP entry"), name);
        if (stat.m_uncomp_size > quint64(MaxFileBytes))
            return fail(error, QStringLiteral("ZIP entry exceeds 5 MiB file limit"), name);
        expanded += stat.m_uncomp_size;
        if (expanded > MaxExpandedBytes)
            return fail(error, QStringLiteral("ZIP exceeds 100 MiB expanded limit"), path);
        if (!directory) {
            regularFiles.insert(key);
            candidate->entries.insert(normalized, i);
        }
    }
    for (auto name : allNames) {
        while (name.contains('/')) {
            name.truncate(name.lastIndexOf('/'));
            if (regularFiles.contains(name))
                return fail(error, QStringLiteral("ZIP file/directory path collision"), name);
        }
    }
    d = std::move(candidate);
    return true;
}
QStringList ZipArchive::files() const { return d->entries.keys(); }
bool ZipArchive::read(const QString& path, QByteArray* data, Error* error) const {
    if (error)
        *error = {};
    if (data)
        data->clear();
    if (!data || !safeName(path))
        return fail(error, QStringLiteral("Unsafe ZIP read path"), path);
    const auto it =
        d->entries.constFind(QDir::cleanPath(path).normalized(QString::NormalizationForm_C));
    if (it == d->entries.cend())
        return fail(error, QStringLiteral("ZIP entry not found"), path);
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&d->zip, it.value(), &stat) ||
        stat.m_uncomp_size > quint64(MaxFileBytes))
        return fail(error, QStringLiteral("ZIP entry exceeds file limit"), path);
    QByteArray result(qsizetype(stat.m_uncomp_size), '\0');
    // miniz checks output size, stream completion and CRC; a dishonest size cannot
    // cause a larger allocation or an unbounded streaming output.
    if (!mz_zip_reader_extract_to_mem(&d->zip, it.value(), result.data(), size_t(result.size()), 0))
        return fail(error, QStringLiteral("ZIP data is corrupt or exceeds declared size"), path);
    *data = std::move(result);
    return true;
}
} // namespace ket::theme::io

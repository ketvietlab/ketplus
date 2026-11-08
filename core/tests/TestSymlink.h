#pragma once

#include <QFile>
#include <QFileInfo>
#include <QString>

#include <filesystem>
#include <system_error>

namespace ketplus::test {

// QFile::link creates Windows shortcuts, which do not exercise filesystem links.
inline QString createSymlink(const QString& target, const QString& link,
                             const bool directory = false) {
    std::error_code error;
    const auto targetPath = QFile(target).filesystemFileName();
    const auto linkPath = QFile(link).filesystemFileName();
    if (directory)
        std::filesystem::create_directory_symlink(targetPath, linkPath, error);
    else
        std::filesystem::create_symlink(targetPath, linkPath, error);
    if (error)
        return QString::fromStdString(error.message());
    if (!QFileInfo(link).isSymbolicLink())
        return QStringLiteral("Fixture is not a filesystem symbolic link");
    return {};
}

} // namespace ketplus::test

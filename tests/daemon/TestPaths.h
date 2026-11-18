#pragma once
#include <QDir>
#include <QString>

// An absolute path that does not exist: "/repo" on POSIX, "C:/repo" on Windows, where a path
// without a drive letter is not absolute.
inline QString absoluteTestPath(const QString& name) { return QDir::rootPath() + name; }

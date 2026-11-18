#pragma once
#include <QDir>
#include <QString>
#include <QTest>

// An absolute path that does not exist: "/repo" on POSIX, "C:/repo" on Windows, where a path
// without a drive letter is not absolute.
inline QString absoluteTestPath(const QString& name) { return QDir::rootPath() + name; }

// A test or data row that cannot run on Windows says why; the reason lands in the test output
// and in docs/testing.md. Elsewhere these expand to nothing.
#if defined(Q_OS_WIN)
#define KETPLUS_SKIP_ON_WINDOWS(reason) QSKIP(reason)
#define KETPLUS_SKIP_ROW_ON_WINDOWS(tag, reason)                                                   \
    do {                                                                                           \
        if (QByteArray(QTest::currentDataTag()) == QByteArray(tag))                                \
            QSKIP(reason);                                                                         \
    } while (false)
#else
#define KETPLUS_SKIP_ON_WINDOWS(reason)                                                            \
    do {                                                                                           \
    } while (false)
#define KETPLUS_SKIP_ROW_ON_WINDOWS(tag, reason)                                                   \
    do {                                                                                           \
    } while (false)
#endif

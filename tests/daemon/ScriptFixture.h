#pragma once

#include <QFile>
#include <QFileDevice>
#include <QString>

// Provider fixtures are Python scripts written to disk and started as the provider's
// executable. POSIX runs them through their `#!/usr/bin/env python3` line. Windows has no
// shebang, and only `.exe`, `.cmd` and `.bat` files count as executable, so there the script
// is moved to `<name>.py` and a small launcher (ScriptLauncher.cpp) is copied to `<name>`; the
// launcher starts the Python interpreter on the script with the same arguments and stdio.

// A fixture's file name: `.exe` is appended on Windows so the launcher counts as executable.
inline QString fixtureName(const QString& name) {
#if defined(Q_OS_WIN)
    return name + QStringLiteral(".exe");
#else
    return name;
#endif
}

// The file that holds a fixture's Python source: the executable itself on POSIX, the `.py` next
// to the launcher on Windows. A test that edits the source of a fixture it wrote edits this file.
inline QString scriptSourcePath(const QString& executable) {
#if defined(Q_OS_WIN)
    return executable + QStringLiteral(".py");
#else
    return executable;
#endif
}

// Makes the script just written to `script` runnable as an executable. `script` must be
// closed or closable; on Windows its file name must end in `.exe` (use fixtureName()).
inline bool finalizeScriptFixture(QFile& script) {
    script.close();
#if defined(Q_OS_WIN) && !defined(KETPLUS_SCRIPT_LAUNCHER)
    return false; // No Python interpreter was found at configure time, so no launcher was built.
#elif defined(Q_OS_WIN)
    const QString path = script.fileName();
    const QString source = path + QStringLiteral(".py");
    QFile::remove(source);
    return path.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive) &&
           QFile::rename(path, source) &&
           QFile::copy(QStringLiteral(KETPLUS_SCRIPT_LAUNCHER), path);
#else
    return script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                 QFileDevice::ExeOwner);
#endif
}

#pragma once
#include <QtGlobal>
#if defined(Q_OS_WIN)
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

// Fixture children write exact byte counts. The Windows C runtime opens stdout/stderr in text
// mode and would turn every "\n" into "\r\n", moving those counts by a byte per line.
inline void makeStdioBinary() {
#if defined(Q_OS_WIN)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
}

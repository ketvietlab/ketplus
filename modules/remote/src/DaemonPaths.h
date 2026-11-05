#pragma once
#include <QString>
namespace ketplus::remote {
struct DaemonPaths final {
    static QString dataDirectory();
    static QString socket();
    static QString state();
    static QString token();
};
} // namespace ketplus::remote

#include "DaemonPaths.h"
#include <QDir>
#include <QStandardPaths>
namespace ketplus::remote {
namespace {
QString configured(const char* name, const QString& fallback) {
    const auto value = qEnvironmentVariable(name).trimmed();
    return value.isEmpty() ? fallback : value;
}
} // namespace
QString DaemonPaths::dataDirectory() {
    return configured("KETPLUS_DAEMON_DATA_DIR",
                      QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation))
                          .filePath("Ketsuite/KetPlus"));
}
QString DaemonPaths::socket() {
    auto runtime = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (runtime.isEmpty())
        runtime = dataDirectory();
    return configured("KETPLUS_DAEMON_SOCKET", QDir(runtime).filePath("ketplus-daemon-v0"));
}
QString DaemonPaths::state() {
    return configured("KETPLUS_DAEMON_STATE",
                      QDir(dataDirectory()).filePath("daemon/journal.sqlite3"));
}
QString DaemonPaths::token() {
    return configured("KETPLUS_DAEMON_TOKEN_FILE",
                      QDir(dataDirectory()).filePath("daemon/local.token"));
}
} // namespace ketplus::remote

#include "exec/LinuxSystemHost.h"

#include "runtime/executor/LinuxJobContainment.h"

#include <QFile>

#include <sys/stat.h>
#include <unistd.h>

namespace ketplus::runner::exec {
namespace {

std::optional<struct stat> statOf(const QString& path, const bool follow) {
    struct stat info{};
    const QByteArray name = QFile::encodeName(path);
    if ((follow ? ::stat(name.constData(), &info) : ::lstat(name.constData(), &info)) != 0)
        return std::nullopt;
    return info;
}

} // namespace

uint LinuxSystemHost::effectiveUid() const { return ::geteuid(); }

QString LinuxSystemHost::bootId() const { return daemon::executor::linuxBootId(); }

bool LinuxSystemHost::executable(const QString& path) const {
    const std::optional<struct stat> info = statOf(path, true);
    return info && S_ISREG(info->st_mode) &&
           ::access(QFile::encodeName(path).constData(), X_OK) == 0;
}

QString LinuxSystemHost::runtimeDirectory() const {
    return qEnvironmentVariable("XDG_RUNTIME_DIR");
}

std::optional<uint> LinuxSystemHost::directoryOwner(const QString& path) const {
    const std::optional<struct stat> info = statOf(path, false);
    if (!info || !S_ISDIR(info->st_mode))
        return std::nullopt;
    return info->st_uid;
}

bool LinuxSystemHost::socket(const QString& path) const {
    const std::optional<struct stat> info = statOf(path, false);
    return info && S_ISSOCK(info->st_mode);
}

bool LinuxSystemHost::file(const QString& path) const {
    const std::optional<struct stat> info = statOf(path, true);
    return info && S_ISREG(info->st_mode);
}

void LinuxSystemHost::probeContainment(const QString& program, std::function<void(bool)> done) {
    daemon::executor::linuxContainmentAvailableAsync(
        program, &m_owner,
        [done = std::move(done)](const bool available, const QString&) { done(available); });
}

void LinuxSystemHost::probeMounts(const QString& program, std::function<void(bool)> done) {
    daemon::executor::linuxMountsAvailableAsync(
        program, &m_owner,
        [done = std::move(done)](const bool available, const QString&) { done(available); });
}

} // namespace ketplus::runner::exec

#include "service/UnixSignals.h"
#if defined(Q_OS_UNIX)
#include <QCoreApplication>
#include <QSocketNotifier>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
namespace ketplus::daemon {
namespace {
volatile sig_atomic_t signalPipe = -1;
void notifySignal(int signal) {
    const int saved = errno;
    const char value = static_cast<char>(signal);
    if (signalPipe >= 0) {
        const auto ignored = ::write(static_cast<int>(signalPipe), &value, 1);
        (void)ignored;
    }
    errno = saved;
}
} // namespace
UnixSignals::UnixSignals(QObject* parent) : QObject(parent) {
    if (::pipe(pipe_) != 0)
        return;
    for (int fd : pipe_) {
        if (::fcntl(fd, F_SETFL, O_NONBLOCK) == -1 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) == -1)
            return;
    }
    struct sigaction action{};
    action.sa_handler = notifySignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_RESTART;
    if (::sigaction(SIGTERM, &action, &previousTerm_) != 0)
        return;
    if (::sigaction(SIGINT, &action, &previousInt_) != 0) {
        ::sigaction(SIGTERM, &previousTerm_, nullptr);
        return;
    }
    signalPipe = pipe_[1];
    ready_ = true;
    auto* notifier = new QSocketNotifier(pipe_[0], QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this] {
        char bytes[32];
        while (::read(pipe_[0], bytes, sizeof(bytes)) > 0) {
        }
        QCoreApplication::quit();
    });
}
UnixSignals::~UnixSignals() {
    if (ready_) {
        signalPipe = -1;
        ::sigaction(SIGTERM, &previousTerm_, nullptr);
        ::sigaction(SIGINT, &previousInt_, nullptr);
    }
    for (int fd : pipe_)
        if (fd >= 0)
            ::close(fd);
}
} // namespace ketplus::daemon
#endif

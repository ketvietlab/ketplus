#pragma once
#include <QObject>
#if defined(Q_OS_WIN)
namespace ketplus::daemon {
// Console Ctrl+C / Ctrl+Break / close / logoff / shutdown events end the event loop, so the
// daemon closes its journal and IPC endpoint instead of being terminated mid-write. The
// Windows counterpart of UnixSignals.
class WindowsShutdown final : public QObject {
  public:
    explicit WindowsShutdown(QObject* parent = nullptr);
    ~WindowsShutdown() override;
    bool isReady() const { return ready_; }

  private:
    bool ready_{false};
};
} // namespace ketplus::daemon
#endif

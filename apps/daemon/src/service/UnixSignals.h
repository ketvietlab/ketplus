#pragma once
#include <QObject>
#if defined(Q_OS_UNIX)
#include <csignal>
namespace ketplus::daemon {
class UnixSignals final : public QObject {
  public:
    explicit UnixSignals(QObject* parent = nullptr);
    ~UnixSignals() override;
    bool isReady() const { return ready_; }

  private:
    int pipe_[2]{-1, -1};
    struct sigaction previousTerm_{};
    struct sigaction previousInt_{};
    bool ready_{false};
};
} // namespace ketplus::daemon
#endif

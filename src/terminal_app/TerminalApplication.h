#pragma once

#include <QApplication>
#include <QStringList>

#include <functional>

namespace ketplus {

// macOS delivers Finder "Open With" requests as QFileOpenEvent instances,
// including requests that arrive before the first application window exists.
class TerminalApplication final : public QApplication {
  public:
    using OpenPathHandler = std::function<void(const QString&)>;

    TerminalApplication(int& argc, char** argv);

    void setOpenPathHandler(OpenPathHandler handler);

  protected:
    bool event(QEvent* event) override;

  private:
    OpenPathHandler openPathHandler_;
    QStringList pendingPaths_;
};

} // namespace ketplus

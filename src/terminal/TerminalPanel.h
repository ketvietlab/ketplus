#pragma once

#include <QWidget>

class QLabel;
class QToolButton;

namespace ketplus {

class PtyProcess;
class TerminalView;
struct ThemePalette;

class TerminalPanel final : public QWidget {
    Q_OBJECT

  public:
    explicit TerminalPanel(QWidget* parent = nullptr);

    void start(const QString& workingDirectory);
    void focusTerminal();
    void applyTheme(const ThemePalette& palette);
    [[nodiscard]] bool isSessionRunning() const;

  signals:
    void closeRequested();
    void statusMessageRequested(const QString& message);

  private:
    PtyProcess* process_{nullptr};
    TerminalView* terminal_{nullptr};
    QLabel* titleLabel_{nullptr};
    QLabel* pathLabel_{nullptr};
    QToolButton* closeButton_{nullptr};
    QString workingDirectory_;
};

} // namespace ketplus

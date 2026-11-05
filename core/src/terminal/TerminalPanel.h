#pragma once

#include <QWidget>

class QLabel;
class QToolButton;

namespace ketplus {

class TerminalSession;
class TerminalView;
struct ThemePalette;

class TerminalPanel final : public QWidget {
    Q_OBJECT

  public:
    explicit TerminalPanel(QWidget* parent = nullptr);

    void start(const QString& workingDirectory);
    void focusTerminal();
    void applyTheme(const ThemePalette& palette);
    void setTypography(int fontSizePixels, int lineHeightPixels);
    [[nodiscard]] bool isSessionRunning() const;

  signals:
    void closeRequested();
    void statusMessageRequested(const QString& message);

  private:
    TerminalSession* session_{nullptr};
    TerminalView* terminal_{nullptr};
    QLabel* titleLabel_{nullptr};
    QLabel* pathLabel_{nullptr};
    QToolButton* findButton_{nullptr};
    QToolButton* clearButton_{nullptr};
    QToolButton* restartButton_{nullptr};
    QToolButton* closeButton_{nullptr};
    QString workingDirectory_;
};

} // namespace ketplus

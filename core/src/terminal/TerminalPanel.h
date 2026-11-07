#pragma once

#include <QWidget>

class QLabel;
class QAbstractButton;

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
    void setTypography(int fontSizePixels, int lineHeightPixels, const QString& fontFamily = {});
    [[nodiscard]] bool isSessionRunning() const;

  signals:
    void closeRequested();
    void statusMessageRequested(const QString& message);

  private:
    TerminalSession* session_{nullptr};
    TerminalView* terminal_{nullptr};
    QLabel* titleLabel_{nullptr};
    QLabel* pathLabel_{nullptr};
    QAbstractButton* findButton_{nullptr};
    QAbstractButton* clearButton_{nullptr};
    QAbstractButton* restartButton_{nullptr};
    QAbstractButton* closeButton_{nullptr};
    QString workingDirectory_;
};

} // namespace ketplus

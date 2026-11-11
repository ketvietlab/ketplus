#pragma once

#include "ui/Theme.h"
#include <QWidget>

class QTabWidget;
class QAbstractButton;

namespace ketplus {

class TerminalTab;
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
    // Types `text` into the current tab without running it (TerminalTab::typeText).
    void typeText(const QString& text);
    // The same in a new tab, since the current one may be running vim or another program. A
    // panel with no tab yet types into its first.
    void typeTextInNewTab(const QString& text);
    // One shell and no tab bar or toolbar, for a host that already gives each terminal its own
    // tab (the Agent Workspace content pane). Find, Clear and Restart stay on the terminal's
    // context menu and shortcuts.
    void setSingleSession(bool single);
    [[nodiscard]] bool isSingleSession() const { return singleSession_; }

  signals:
    void closeRequested();
    void statusMessageRequested(const QString& message);

  private:
    TerminalTab* currentTab() const;
    void addTab(bool start);
    QTabWidget* tabs_{nullptr};
    QWidget* actions_{nullptr};
    bool singleSession_{false};
    ThemePalette palette_;
    int fontSizePixels_{14};
    int lineHeightPixels_{22};
    QString fontFamily_;
    QString workingDirectory_;
};

} // namespace ketplus

#pragma once

#include "terminal/TerminalSession.h"

#include <QWidget>

namespace ketplus {

class TerminalView;
struct ThemePalette;

class TerminalTab final : public QWidget {
    Q_OBJECT

  public:
    explicit TerminalTab(const QString& workingDirectory, const QString& commandPath = {},
                         QWidget* parent = nullptr);
    ~TerminalTab() override;

    [[nodiscard]] QString workingDirectory() const;
    [[nodiscard]] QString title() const;
    [[nodiscard]] TerminalView* view() const noexcept;

    void start();
    void setDirectoryBeforeStart(const QString& path);
    [[nodiscard]] bool isRunning() const { return session_.isRunning(); }
    bool restart();
    void stop();
    // Types `text` at the prompt as a paste, without Return, so the user decides to run it. Text
    // typed before the shell starts waits for it.
    void typeText(const QString& text);
    void applyTheme(const ThemePalette& palette);
    void setTypography(int fontSizePixels, int lineHeightPixels, const QString& fontFamily = {});

  signals:
    void titleChanged(const QString& title);
    void statusMessageRequested(const QString& message);

  private:
    void setTitle(const QString& title);
    [[nodiscard]] QByteArray commandForPath(const QString& path) const;

    TerminalSession session_;
    TerminalView* terminal_{nullptr};
    QString workingDirectory_;
    QString pendingCommandPath_;
    QString pendingTypedText_;
    QString title_;
    bool started_{false};
};

} // namespace ketplus

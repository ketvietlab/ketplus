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
    bool restart();
    void stop();
    void applyTheme(const ThemePalette& palette);
    void setTypography(int fontSizePixels, int lineHeightPixels);

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
    QString title_;
    bool started_{false};
};

} // namespace ketplus

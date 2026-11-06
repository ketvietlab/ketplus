// SPDX-License-Identifier: MPL-2.0
#pragma once

#include <QMainWindow>

class QTabWidget;

namespace ketplus {
class TerminalTab;
}

// A KetPlus Terminal window drawn with the Két Qt kit around the shipped terminal tab: the
// menus and shortcuts of core/src/terminal_app/TerminalWindow, document tabs with find, clear
// and restart beside them, and status messages as Két toasts.
class TerminalMockWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit TerminalMockWindow(QString workingDirectory, QWidget* parent = nullptr);

    void addTab(const QString& workingDirectory, const QString& commandPath = {});
    [[nodiscard]] ketplus::TerminalTab* currentTab() const;

  signals:
    void newWindowRequested(const QString& workingDirectory);

  private:
    void createMenus();
    void closeTab(int index);
    void activateAdjacentTab(int delta);
    void changeFontSize(int delta);
    void applyTheme();
    void applyTypography(ketplus::TerminalTab* tab) const;
    void showStatus(const QString& message);
    void updateWindowTitle();

    QTabWidget* tabs_;
    QString workingDirectory_;
    int fontSizePixels_ = 14;
    int lineHeightPixels_ = 22;
};

#pragma once

#include <QMainWindow>
#include <QString>

class QCloseEvent;
class QTabWidget;

namespace ketplus {

class TerminalTab;
class ThemeManager;

class TerminalWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit TerminalWindow(ThemeManager& theme, const QString& workingDirectory,
                            const QString& commandPath = {}, QWidget* parent = nullptr);

  signals:
    void newWindowRequested(const QString& workingDirectory);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void createMenus();
    void addTab(const QString& workingDirectory, const QString& commandPath = {});
    void closeTab(int index);
    void activateAdjacentTab(int delta);
    void applyTheme();
    void restartSession();
    void changeFontSize(int delta);
    void updateWindowTitle();
    [[nodiscard]] TerminalTab* currentTab() const;

    ThemeManager& theme_;
    QTabWidget* tabs_{nullptr};
    QString workingDirectory_;
    int fontSizePixels_{14};
    int lineHeightPixels_{22};
};

} // namespace ketplus

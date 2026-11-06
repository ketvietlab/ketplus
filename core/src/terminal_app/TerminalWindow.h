#pragma once

#include <QMainWindow>
#include <QString>

class QCloseEvent;
class QTabWidget;

namespace ketplus {

class TerminalTab;

// Applies the appearance saved under appearance/theme ("system", "light" or "dark") to
// ket::Appearance. Call once, before the first window.
void restoreTerminalAppearance();

// A KetPlus Terminal window drawn with the Két Qt kit: document tabs with New tab, Find, Clear
// scrollback and Restart beside them, status messages as Két toasts, and colours from Két DS
// tokens. ket::Style must be installed on the application.
class TerminalWindow final : public QMainWindow {
    Q_OBJECT

  public:
    explicit TerminalWindow(const QString& workingDirectory, const QString& commandPath = {},
                            QWidget* parent = nullptr);

    void addTab(const QString& workingDirectory, const QString& commandPath = {});
    [[nodiscard]] TerminalTab* currentTab() const;

  signals:
    void newWindowRequested(const QString& workingDirectory);

  protected:
    void closeEvent(QCloseEvent* event) override;

  private:
    void createMenus();
    void closeTab(int index);
    void activateAdjacentTab(int delta);
    void applyTheme();
    void applyTypography(TerminalTab* tab) const;
    void restartSession();
    void changeFontSize(int delta);
    void showStatus(const QString& message);
    void updateWindowTitle();

    QTabWidget* tabs_{nullptr};
    QString workingDirectory_;
    int fontSizePixels_{14};
    int lineHeightPixels_{22};
};

} // namespace ketplus

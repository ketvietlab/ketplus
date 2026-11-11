#pragma once

#include <QHash>
#include <QSet>
#include <QString>
#include <QWidget>

class QGridLayout;
class QLabel;
class QListWidget;
class QStackedWidget;
class QVBoxLayout;

namespace ketplus {

class SettingsDialog;

// Settings as a page of the window, as the mock draws it (mockups/qt SettingsScreen): the
// heading with its shortcut, the section navigation, Appearance and Typography composed from
// the dialog's own controls, and the dialog's Restore defaults below. Every change applies as
// it is made (ADR 0103). Colours come from the kit's tokens for the current theme and lengths
// follow the interface zoom. KétPlus shows it as is; Pro adds its sections in
// SettingsWorkspacePage.
class SettingsWorkspace : public QWidget {
    Q_OBJECT

  public:
    // `shortcut` is the Settings shortcut shown beside the title, such as "⌘ ,";
    // `interfaceFont` names the interface font on Typography.
    SettingsWorkspace(SettingsDialog* dialog, const QString& shortcut, const QString& interfaceFont,
                      QWidget* parent = nullptr);
    ~SettingsWorkspace() override;

    [[nodiscard]] SettingsDialog* dialog() const noexcept { return dialog_; }
    // Selects a section by its navigation id, such as "typography".
    void showSection(const QString& id);
    [[nodiscard]] QString currentSection() const;

    // A label of the page's type roles (eyebrow, title, help, field label…), by object name.
    static QLabel* label(const QString& text, const QString& objectName, QWidget* parent);

  signals:
    void saved();
    void dismissed();
    // A navigation entry added with addLink().
    void linkActivated(const QString& id);

  protected:
    // A section after the ones already added; `restore` keeps Restore defaults below it.
    void addSection(const QString& title, const QString& id, QWidget* page, bool restore = true);
    // An entry that leaves Settings, such as Pro's Account & devices.
    void addLink(const QString& title, const QString& id);
    // A Typography row. Rows 0 and 1 hold the interface font and size, 3 and 4 the editor's.
    void addTypographyRow(int row, const QString& title, const QString& help, QWidget* control);
    // Stylesheet rules for the product's own sections, with the page's colours: %1 page,
    // %2 text, %3 muted text, %4 border and %5 accent text.
    void addStyleRules(const QString& rules);

  private:
    void addNavigation(const QString& title, const QString& id);
    QWidget* createAppearance();
    QWidget* createTypography(const QString& interfaceFont);
    void showRow(int row);
    void applyAppearance();

    SettingsDialog* dialog_;
    QListWidget* navigation_{nullptr};
    QStackedWidget* pages_{nullptr};
    QGridLayout* typographyFields_{nullptr};
    QWidget* footer_{nullptr};
    QHash<QString, QWidget*> sections_;
    QSet<QString> withoutRestore_;
    QString styleRules_;
};

} // namespace ketplus

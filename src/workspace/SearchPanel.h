#pragma once

#include "workspace/WorkspaceSearch.h"

#include <QStringList>
#include <QWidget>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace ketplus {

// Sidebar for searching and replacing across workspace files.
class SearchPanel final : public QWidget {
    Q_OBJECT

  public:
    explicit SearchPanel(QWidget* parent = nullptr);

    [[nodiscard]] WorkspaceSearchOptions options() const;
    // Shows the query that produced the listed results, e.g. after a definition lookup.
    void setOptions(const WorkspaceSearchOptions& options);
    [[nodiscard]] QString replacement() const;
    [[nodiscard]] QStringList resultPaths() const;
    [[nodiscard]] int resultMatchCount() const noexcept;
    [[nodiscard]] bool isSearching() const noexcept;

    void focusQuery(const QString& seed = {});
    void clearResults();
    void addFileResult(const WorkspaceSearchFileResult& result);
    void setSearching(bool searching);
    void setStatusText(const QString& text);
    // Colors for the result rows, so a folder, a file name, a line number and the matched
    // text are told apart. Called with the theme's palette.
    void setResultColors(const QString& folder, const QString& fileName, const QString& lineNumber,
                         const QString& text, const QString& match);

  signals:
    void searchRequested();
    void cancelRequested();
    void replaceAllRequested();
    void matchActivated(const QString& path, int line, int column, int length);
    void hideRequested();

  private:
    void activateItem(QTreeWidgetItem* item);
    [[nodiscard]] QString matchMarkup(const WorkspaceSearchMatch& match) const;

    QLineEdit* queryEdit_{nullptr};
    QLineEdit* replaceEdit_{nullptr};
    QLineEdit* includeEdit_{nullptr};
    QCheckBox* matchCaseCheck_{nullptr};
    QCheckBox* wholeWordCheck_{nullptr};
    QCheckBox* regexCheck_{nullptr};
    QPushButton* searchButton_{nullptr};
    QPushButton* replaceAllButton_{nullptr};
    QLabel* statusLabel_{nullptr};
    QTreeWidget* results_{nullptr};
    struct ResultColors final {
        QString folder{QStringLiteral("#8B93A1")};
        QString fileName{QStringLiteral("#E6EDF3")};
        QString lineNumber{QStringLiteral("#6E7681")};
        QString text{QStringLiteral("#ADBAC7")};
        QString match{QStringLiteral("#E5A93C")};
    } colors_;
    int matchCount_{0};
    bool searching_{false};
};

} // namespace ketplus

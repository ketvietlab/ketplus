#pragma once

#include "editor/EditorSettings.h"
#include "git/GitTypes.h"
#include "ui/Theme.h"

#include <QWidget>

class QLabel;
class QTableView;
class QToolButton;

namespace ketplus {

class EditorWidget;
class GitDiffModel;

class GitDiffView final : public QWidget {
    Q_OBJECT

  public:
    explicit GitDiffView(QWidget* parent = nullptr);

    [[nodiscard]] QString filePath() const;
    [[nodiscard]] GitDiffMode mode() const noexcept;
    void setDiff(const QString& filePath, GitDiffMode mode, const QString& diff,
                 const ThemePalette& palette);
    void applyEditorSettings(const EditorSettings& settings);
    void applyTheme(const ThemePalette& palette);

  signals:
    void openFileRequested(const QString& path);
    void refreshRequested(const QString& path, ketplus::GitDiffMode mode);

  private:
    void updateHeader();
    void updateSpans();
    void updateContextButton();

    GitDiffModel* model_{nullptr};
    QTableView* table_{nullptr};
    EditorWidget* syntaxEngine_{nullptr};
    QLabel* fileLabel_{nullptr};
    QLabel* pathLabel_{nullptr};
    QLabel* scopeLabel_{nullptr};
    QLabel* statsLabel_{nullptr};
    QToolButton* openButton_{nullptr};
    QToolButton* contextButton_{nullptr};
    QString filePath_;
    QString rawDiff_;
    ThemePalette palette_;
    GitDiffMode mode_{GitDiffMode::Combined};
    bool hideUnchangedRows_{true};
};

} // namespace ketplus

#pragma once

#include <QHash>
#include <QList>
#include <QPointer>
#include <QWidget>

class QTabBar;

namespace ketplus {

class EditorWidget;

// The second editor group: one Scintilla view that shows any of several source tabs.
// Each tab shares its source's document, so no text is copied or reloaded.
class EditorSplitPane final : public QWidget {
    Q_OBJECT

  public:
    explicit EditorSplitPane(QWidget* parent = nullptr);

    [[nodiscard]] EditorWidget* editor() const noexcept;
    [[nodiscard]] EditorWidget* currentSource() const;
    [[nodiscard]] QList<EditorWidget*> sources() const;
    [[nodiscard]] bool containsSource(const EditorWidget* source) const;
    [[nodiscard]] QTabBar* tabBar() const noexcept;

    // Adds a tab for the source when needed and makes it current.
    void showSource(EditorWidget* source);
    // Removes the source's tab; emits emptied() after the last one.
    void removeSource(EditorWidget* source);
    // Re-shares the current source, e.g. after Save As picked another lexer.
    void refreshCurrentSource();
    void updateSourceTitle(EditorWidget* source);

  signals:
    // The view now shows this source's document; callers reapply the theme.
    void currentSourceChanged(EditorWidget* source);
    void emptied();

  private:
    struct ViewState final {
        long long caret{0};
        long long anchor{0};
        long long firstVisibleLine{0};
    };

    [[nodiscard]] int indexOfSource(const EditorWidget* source) const;
    void activateIndex(int index);
    void saveViewState();
    void restoreViewState(EditorWidget* source);
    void addCloseButton(int index, EditorWidget* source);

    QTabBar* tabBar_{nullptr};
    EditorWidget* editor_{nullptr};
    QPointer<EditorWidget> current_;
    QHash<const EditorWidget*, ViewState> viewStates_;
    bool switching_{false};
};

} // namespace ketplus

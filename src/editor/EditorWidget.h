#pragma once

#include "core/Document.h"
#include "editor/EditorSettings.h"

#include <ScintillaEditBase.h>

#include <cstdint>
#include <memory>

class QContextMenuEvent;
class QPoint;

namespace ketplus {

struct ThemePalette;

struct FindResult final {
    bool found{false};
    bool wrapped{false};
};

class EditorWidget final : public ScintillaEditBase {
    Q_OBJECT

  public:
    static constexpr qsizetype largeFileThresholdBytes = 10 * 1024 * 1024;

    explicit EditorWidget(QWidget* parent = nullptr);

    [[nodiscard]] Document& document() noexcept;
    [[nodiscard]] const Document& document() const noexcept;
    [[nodiscard]] QByteArray text() const;
    [[nodiscard]] QString selectedText() const;
    [[nodiscard]] bool canUndoEdit() const;
    [[nodiscard]] bool canRedoEdit() const;
    [[nodiscard]] bool canPasteEdit() const;
    [[nodiscard]] bool hasSelection() const;
    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] QString syntaxName() const;
    [[nodiscard]] bool isLargeFileMode() const noexcept;
    [[nodiscard]] bool isHibernated() const noexcept;
    [[nodiscard]] bool hasPendingTheme() const noexcept;
    [[nodiscard]] qsizetype residentBytes() const;
    [[nodiscard]] std::uint64_t lastActivated() const noexcept;

    void setText(const QByteArray& text);
    [[nodiscard]] Document::Result loadFile(const QString& filePath);
    [[nodiscard]] Document::Result restoreFromDisk();
    [[nodiscard]] bool hibernate();
    void markThemePending() noexcept;
    void markActivated(std::uint64_t sequence) noexcept;
    void markSaved();
    void configureLexerForPath(const QString& filePath);
    void setEditorSettings(const EditorSettings& settings);
    [[nodiscard]] const EditorSettings& editorSettings() const noexcept;
    void applyTheme(const ThemePalette& palette);
    void undoEdit();
    void redoEdit();
    void cutSelection();
    void copySelection();
    void pasteClipboard();
    void deleteSelection();
    void selectAllText();
    FindResult findText(const QString& query, bool backwards, bool matchCase, bool wholeWord);
    bool replaceSelection(const QString& query, const QString& replacement, bool matchCase,
                          bool wholeWord);
    int replaceAll(const QString& query, const QString& replacement, bool matchCase,
                   bool wholeWord);

  signals:
    void dirtyStateChanged(bool dirty);
    void cursorPositionChanged(int line, int column);
    void editorStateChanged();
    void contextMenuRequested(const QPoint& position);

  protected:
    void contextMenuEvent(QContextMenuEvent* event) override;

  private:
    struct ViewState final {
        sptr_t caret{0};
        sptr_t anchor{0};
        sptr_t firstVisibleLine{0};
        sptr_t horizontalOffset{0};
    };

    void configureEditor();
    void updateLineNumberMarginWidth();
    void updateLargeFileMode(qsizetype contentSize);
    void restoreViewState();
    void applyLexerTheme(const ThemePalette& palette);
    void setSearchOptions(bool matchCase, bool wholeWord);
    bool selectionMatches(const QByteArray& query, bool matchCase, bool wholeWord);

    std::unique_ptr<Document> document_;
    EditorSettings editorSettings_{EditorSettings::defaults()};
    QString lexerName_;
    QString syntaxName_;
    ViewState hibernatedViewState_;
    std::uint64_t lastActivated_{0};
    bool largeFileMode_{false};
    bool hibernated_{false};
    bool themePending_{false};
    bool internalMutation_{false};
    int lineNumberDigits_{0};
};

} // namespace ketplus

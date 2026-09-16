#pragma once

#include "core/Document.h"
#include "editor/EditorSettings.h"
#include "editor/EditorViewOptions.h"

#include <ScintillaEditBase.h>

#include <QList>

#include <array>
#include <cstdint>
#include <memory>
#include <utility>

class QContextMenuEvent;
class QEvent;
class QMouseEvent;
class QPoint;

namespace ketplus {

struct ThemePalette;

struct FindResult final {
    bool found{false};
    bool wrapped{false};
};

struct SearchOptions final {
    bool matchCase{false};
    bool wholeWord{false};
    bool regex{false};
    bool inSelection{false};
};

class EditorWidget final : public ScintillaEditBase {
    Q_OBJECT

  public:
    static constexpr qsizetype largeFileThresholdBytes = 10 * 1024 * 1024;
    static constexpr int highlightMatchLimit = 5000;
    static constexpr int selectionMatchLimit = 500;
    static constexpr int minimumCompletionPrefix = 3;
    static constexpr int maximumCompletionItems = 100;

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
    // Forces a language by syntax name; an empty name returns to detection by file name.
    void setSyntaxOverride(const QString& syntaxName);
    [[nodiscard]] QString syntaxOverride() const;
    [[nodiscard]] LineEnding lineEnding() const;
    // Converts every line break in the document as one undo step.
    void setLineEnding(LineEnding lineEnding);
    [[nodiscard]] Document::Result reloadWithEncoding(TextEncoding encoding);
    // Replaces the whole text as one undo step, keeping the document open.
    void replaceAllText(const QByteArray& text);
    // Selects `length` UTF-16 characters starting at a 1-based line and 0-based column.
    void selectTextRange(int line, int column, int length);
    int highlightSelectionMatches();
    void setEditorSettings(const EditorSettings& settings);
    [[nodiscard]] const EditorSettings& editorSettings() const noexcept;
    void setViewOptions(const EditorViewOptions& options);
    [[nodiscard]] const EditorViewOptions& viewOptions() const noexcept;
    void shareDocumentWith(const EditorWidget& source);
    void applyTheme(const ThemePalette& palette);
    void undoEdit();
    void redoEdit();
    void cutSelection();
    void copySelection();
    void pasteClipboard();
    void deleteSelection();
    void selectAllText();

    void duplicateLines();
    void moveLinesUp();
    void moveLinesDown();
    void deleteLines();
    void joinLines();
    void sortLines(bool removeDuplicates);
    void trimTrailingWhitespace();
    void convertCase(bool upper);
    bool toggleComment();

    [[nodiscard]] int selectionCount() const;
    void addNextOccurrence();
    void selectAllOccurrences();
    void addCursorVertically(bool above);
    void splitSelectionIntoLines();
    void collapseToMainSelection();
    bool showWordCompletions(bool explicitRequest);

    [[nodiscard]] int lineCount() const;
    [[nodiscard]] int currentLine() const;
    void goToLine(int line);
    [[nodiscard]] sptr_t caretPosition() const;
    void setCaretPosition(sptr_t position);
    bool jumpToMatchingBrace();
    void toggleBookmark();
    [[nodiscard]] bool hasBookmark(int line) const;
    bool goToNextBookmark();
    bool goToPreviousBookmark();
    void clearBookmarks();
    void setAllFoldsExpanded(bool expanded);

    FindResult findText(const QString& query, bool backwards, bool matchCase, bool wholeWord);
    FindResult findText(const QString& query, bool backwards, const SearchOptions& options);
    bool replaceSelection(const QString& query, const QString& replacement, bool matchCase,
                          bool wholeWord);
    bool replaceSelection(const QString& query, const QString& replacement,
                          const SearchOptions& options);
    int replaceAll(const QString& query, const QString& replacement, bool matchCase,
                   bool wholeWord);
    int replaceAll(const QString& query, const QString& replacement, const SearchOptions& options);
    void setSearchScopeToSelection();
    void clearSearchScope() noexcept;
    [[nodiscard]] bool hasSearchScope() const noexcept;
    int highlightMatches(const QString& query, const SearchOptions& options);
    void clearMatchHighlights();

    // The word and the file-reference token around a position, for go to definition.
    [[nodiscard]] QString wordAtPosition(sptr_t position) const;
    [[nodiscard]] QString fileTokenAtPosition(sptr_t position) const;
    [[nodiscard]] sptr_t caretWordPosition() const;

  signals:
    // Emitted on Ctrl/Cmd+click, with the word and the file token under the pointer.
    void definitionRequested(const QString& symbol, const QString& fileToken);
    void dirtyStateChanged(bool dirty);
    void cursorPositionChanged(int line, int column);
    void editorStateChanged();
    void contextMenuRequested(const QPoint& position);

  protected:
    void contextMenuEvent(QContextMenuEvent* event) override;
    // Watches the viewport for pointer moves so the fold margin can follow the mouse.
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    struct ViewState final {
        sptr_t caret{0};
        sptr_t anchor{0};
        sptr_t firstVisibleLine{0};
        sptr_t horizontalOffset{0};
    };

    struct LineRange final {
        sptr_t first{0};
        sptr_t last{0};
    };

    void configureEditor();
    void updateLineNumberMarginWidth();
    void updateLargeFileMode(qsizetype contentSize);
    void restoreViewState();
    void applyLexerTheme(const ThemePalette& palette);
    // Fold arrows for open blocks only appear while the pointer is over the margins.
    void setFoldMarginHovered(bool hovered);
    void defineFoldMarkers();
    // Underlines the word under the pointer while Ctrl/Cmd is held.
    void updateLinkHighlight(const QPoint& point, Qt::KeyboardModifiers modifiers);
    void clearLinkHighlight();
    void updateEmbeddedStyleHighlight();
    bool applyViewOptions();
    void updateBraceHighlight();
    void handleCharAdded(int character);
    void autoIndentCurrentLine();
    void toggleBookmarkAtLine(sptr_t line);
    void setSearchOptions(const SearchOptions& options);
    bool selectionMatches(const QByteArray& query, const SearchOptions& options);
    [[nodiscard]] std::pair<sptr_t, sptr_t> searchBounds(const SearchOptions& options) const;
    sptr_t replaceTarget(const QByteArray& replacement, bool regex);
    void adjustSearchScope(sptr_t delta);
    [[nodiscard]] int charAt(sptr_t position) const;
    [[nodiscard]] QByteArray textRange(sptr_t start, sptr_t end) const;
    [[nodiscard]] QByteArray endOfLine() const;
    [[nodiscard]] LineRange selectedLineRange(bool wholeDocumentWhenEmpty) const;
    [[nodiscard]] QList<QByteArray> linesInRange(LineRange range) const;
    void replaceLines(LineRange range, const QList<QByteArray>& lines);

    std::unique_ptr<Document> document_;
    EditorSettings editorSettings_{EditorSettings::defaults()};
    EditorViewOptions viewOptions_;
    QString foldMarkerColor_{QStringLiteral("#8B8B8B")};
    QString lexerName_;
    QString syntaxName_;
    QString syntaxOverride_;
    ViewState hibernatedViewState_;
    std::uint64_t lastActivated_{0};
    sptr_t searchScopeStart_{0};
    sptr_t searchScopeEnd_{0};
    sptr_t highlightedBrace_{-1};
    sptr_t highlightedBraceMatch_{-1};
    bool largeFileMode_{false};
    bool hibernated_{false};
    bool themePending_{false};
    bool internalMutation_{false};
    bool foldingEnabled_{false};
    bool searchScopeActive_{false};
    bool selectionMatchesActive_{false};
    sptr_t linkStart_{-1};
    sptr_t linkEnd_{-1};
    bool foldMarginHovered_{false};
    bool embeddedStyleActive_{false};
    // The last colored range; text edits and theme or lexer changes force a rescan.
    bool embeddedStyleDirty_{true};
    sptr_t embeddedStyleStart_{-1};
    sptr_t embeddedStyleEnd_{-1};
    int lineNumberDigits_{0};
    std::array<int, 11> embeddedStyleColors_{};
};

} // namespace ketplus

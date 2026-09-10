#include "editor/EditorWidget.h"

#include "editor/SyntaxDefinition.h"
#include "ui/Theme.h"

#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>
#include <ScintillaMessages.h>

#include <QColor>
#include <QContextMenuEvent>
#include <QtMath>

#include <initializer_list>
#include <limits>

namespace ketplus {
namespace {

constexpr unsigned int message(const Scintilla::Message value) {
    return static_cast<unsigned int>(value);
}

int scintillaColor(const QString& value) {
    const QColor color(value);
    return color.red() | (color.green() << 8) | (color.blue() << 16);
}

void applyStyle(ScintillaEditBase& editor, const int style, const QString& foreground,
                const bool bold = false, const bool italic = false) {
    editor.send(message(Scintilla::Message::StyleSetFore), static_cast<uptr_t>(style),
                scintillaColor(foreground));
    editor.send(message(Scintilla::Message::StyleSetBold), static_cast<uptr_t>(style), bold);
    editor.send(message(Scintilla::Message::StyleSetItalic), static_cast<uptr_t>(style), italic);
}

void setStyles(ScintillaEditBase& editor, const std::initializer_list<int> styles,
               const QString& foreground, const bool bold = false, const bool italic = false) {
    for (const int style : styles) {
        applyStyle(editor, style, foreground, bold, italic);
    }
}

} // namespace

EditorWidget::EditorWidget(QWidget* parent)
    : ScintillaEditBase(parent), document_(std::make_unique<Document>()) {
    configureEditor();
    connect(this, &ScintillaEditBase::savePointChanged, this, [this](const bool dirty) {
        if (internalMutation_) {
            return;
        }
        document_->setModified(dirty);
        emit dirtyStateChanged(dirty);
    });
    connect(this, &ScintillaEditBase::updateUi, this, [this](Scintilla::Update) {
        const auto position = send(message(Scintilla::Message::GetCurrentPos));
        const auto line = send(message(Scintilla::Message::LineFromPosition), position);
        const auto column = send(message(Scintilla::Message::GetColumn), position);
        emit cursorPositionChanged(static_cast<int>(line + 1), static_cast<int>(column + 1));
        emit editorStateChanged();
    });
    connect(this, &ScintillaEditBase::linesAdded, this,
            [this](Scintilla::Position) { updateLineNumberMarginWidth(); });
}

void EditorWidget::contextMenuEvent(QContextMenuEvent* event) {
    const auto position = send(message(Scintilla::Message::PositionFromPointClose),
                               static_cast<uptr_t>(event->pos().x()),
                               static_cast<sptr_t>(event->pos().y()));
    const auto selectionStart = send(message(Scintilla::Message::GetSelectionStart));
    const auto selectionEnd = send(message(Scintilla::Message::GetSelectionEnd));
    if (position >= 0 && (position < selectionStart || position > selectionEnd)) {
        send(message(Scintilla::Message::SetEmptySelection),
             static_cast<uptr_t>(position));
    }
    emit contextMenuRequested(event->pos());
    event->accept();
}

Document& EditorWidget::document() noexcept { return *document_; }

const Document& EditorWidget::document() const noexcept { return *document_; }

QByteArray EditorWidget::text() const {
    const auto length = send(message(Scintilla::Message::GetTextLength));
    QByteArray value(static_cast<qsizetype>(length) + 1, '\0');
    send(message(Scintilla::Message::GetText), static_cast<uptr_t>(value.size()),
         reinterpret_cast<sptr_t>(value.data()));
    value.chop(1);
    return value;
}

QString EditorWidget::selectedText() const {
    const auto length = send(message(Scintilla::Message::GetSelText));
    if (length <= 0) {
        return {};
    }
    QByteArray value(static_cast<qsizetype>(length) + 1, '\0');
    send(message(Scintilla::Message::GetSelText), 0, reinterpret_cast<sptr_t>(value.data()));
    value.resize(static_cast<qsizetype>(length));
    return QString::fromUtf8(value);
}

bool EditorWidget::canUndoEdit() const { return send(message(Scintilla::Message::CanUndo)) != 0; }

bool EditorWidget::canRedoEdit() const { return send(message(Scintilla::Message::CanRedo)) != 0; }

bool EditorWidget::canPasteEdit() const { return send(message(Scintilla::Message::CanPaste)) != 0; }

bool EditorWidget::hasSelection() const {
    return send(message(Scintilla::Message::GetSelectionStart)) !=
           send(message(Scintilla::Message::GetSelectionEnd));
}

bool EditorWidget::isEmpty() const { return send(message(Scintilla::Message::GetLength)) == 0; }

QString EditorWidget::syntaxName() const { return syntaxName_; }

bool EditorWidget::isLargeFileMode() const noexcept { return largeFileMode_; }

bool EditorWidget::isHibernated() const noexcept { return hibernated_; }

bool EditorWidget::hasPendingTheme() const noexcept { return themePending_; }

qsizetype EditorWidget::residentBytes() const {
    return hibernated_ ? 0 : static_cast<qsizetype>(send(message(Scintilla::Message::GetLength)));
}

std::uint64_t EditorWidget::lastActivated() const noexcept { return lastActivated_; }

void EditorWidget::setText(const QByteArray& text) {
    internalMutation_ = true;
    updateLargeFileMode(text.size());
    send(message(Scintilla::Message::SetUndoCollection), largeFileMode_ ? 0 : 1);
    sends(message(Scintilla::Message::SetText), 0, text.constData());
    send(message(Scintilla::Message::EmptyUndoBuffer));
    markSaved();
    hibernated_ = false;
    internalMutation_ = false;
    updateLineNumberMarginWidth();
}

Document::Result EditorWidget::loadFile(const QString& filePath) {
    auto result = document_->load(filePath);
    if (!result.ok) {
        return {false, result.error};
    }

    setText(result.content);
    configureLexerForPath(document_->filePath());
    return {true, {}};
}

Document::Result EditorWidget::restoreFromDisk() {
    if (!hibernated_ || document_->isUntitled()) {
        return {true, {}};
    }

    const auto path = document_->filePath();
    auto result = document_->load(path);
    if (!result.ok) {
        return {false, result.error};
    }

    setText(result.content);
    configureLexerForPath(path);
    restoreViewState();
    return {true, {}};
}

bool EditorWidget::hibernate() {
    if (hibernated_ || document_->isUntitled() || document_->isModified()) {
        return false;
    }

    hibernatedViewState_ = {
        .caret = send(message(Scintilla::Message::GetCurrentPos)),
        .anchor = send(message(Scintilla::Message::GetAnchor)),
        .firstVisibleLine = send(message(Scintilla::Message::GetFirstVisibleLine)),
        .horizontalOffset = send(message(Scintilla::Message::GetXOffset)),
    };

    internalMutation_ = true;
    send(message(Scintilla::Message::SetILexer), 0, 0);
    send(message(Scintilla::Message::SetUndoCollection), 0);
    sends(message(Scintilla::Message::SetText), 0, "");
    send(message(Scintilla::Message::EmptyUndoBuffer));
    send(message(Scintilla::Message::SetSavePoint));
    document_->setModified(false);
    hibernated_ = true;
    internalMutation_ = false;
    return true;
}

void EditorWidget::markThemePending() noexcept { themePending_ = true; }

void EditorWidget::markActivated(const std::uint64_t sequence) noexcept {
    lastActivated_ = sequence;
}

void EditorWidget::markSaved() {
    send(message(Scintilla::Message::SetSavePoint));
    document_->setModified(false);
}

void EditorWidget::configureLexerForPath(const QString& filePath) {
    updateLargeFileMode(
        static_cast<qsizetype>(send(message(Scintilla::Message::GetLength))));
    if (largeFileMode_) {
        lexerName_ = QStringLiteral("null");
        syntaxName_ = QStringLiteral("large-file");
        Scintilla::ILexer5* lexer = CreateLexer("null");
        send(message(Scintilla::Message::SetILexer), 0, reinterpret_cast<sptr_t>(lexer));
        send(message(Scintilla::Message::Colourise), 0, -1);
        return;
    }

    const auto& definition = syntaxDefinitionForPath(filePath);
    lexerName_ = QString::fromLatin1(definition.lexer);
    syntaxName_ = QString::fromLatin1(definition.name);
    Scintilla::ILexer5* lexer = CreateLexer(definition.lexer);
    send(message(Scintilla::Message::SetILexer), 0, reinterpret_cast<sptr_t>(lexer));
    for (std::size_t index = 0; index < definition.keywordSets.size(); ++index) {
        const char* keywords = definition.keywordSets[index];
        sends(message(Scintilla::Message::SetKeyWords), static_cast<uptr_t>(index),
              keywords == nullptr ? "" : keywords);
    }
    send(message(Scintilla::Message::Colourise), 0, -1);
}

void EditorWidget::setEditorSettings(const EditorSettings& settings) {
    editorSettings_ = settings.normalized();
}

const EditorSettings& EditorWidget::editorSettings() const noexcept {
    return editorSettings_;
}

void EditorWidget::applyTheme(const ThemePalette& palette) {
    const auto family = editorSettings_.fontFamily.toUtf8();
    const int sizeHundredthPoints =
        qRound(editorSettings_.fontSizePixels * 72.0 * 100.0 / logicalDpiY());

    sends(message(Scintilla::Message::StyleSetFont), STYLE_DEFAULT, family.constData());
    send(message(Scintilla::Message::StyleSetSizeFractional), STYLE_DEFAULT,
         sizeHundredthPoints);
    send(message(Scintilla::Message::StyleSetFore), STYLE_DEFAULT,
         scintillaColor(palette.textMain));
    send(message(Scintilla::Message::StyleSetBack), STYLE_DEFAULT,
         scintillaColor(palette.panelBackground));
    send(message(Scintilla::Message::StyleClearAll));

    // Scintilla sizes fonts in points, so convert from logical pixels and add
    // balanced leading after measuring the selected font's native line height.
    send(message(Scintilla::Message::SetExtraAscent), 0);
    send(message(Scintilla::Message::SetExtraDescent), 0);
    const int nativeLineHeight =
        static_cast<int>(send(message(Scintilla::Message::TextHeight), 0));
    const int extraLeading = qMax(0, editorSettings_.lineHeightPixels - nativeLineHeight);
    send(message(Scintilla::Message::SetExtraAscent), extraLeading / 2);
    send(message(Scintilla::Message::SetExtraDescent), extraLeading - extraLeading / 2);

    applyStyle(*this, STYLE_LINENUMBER, palette.textMuted);
    send(message(Scintilla::Message::StyleSetBack), STYLE_LINENUMBER,
         scintillaColor(palette.panelSubtle));
    lineNumberDigits_ = 0;
    updateLineNumberMarginWidth();
    applyStyle(*this, STYLE_BRACELIGHT, palette.accent, true);
    send(message(Scintilla::Message::StyleSetBack), STYLE_BRACELIGHT,
         scintillaColor(palette.accentSubtle));
    applyStyle(*this, STYLE_BRACEBAD, palette.danger, true);

    send(message(Scintilla::Message::SetSelFore), 1, scintillaColor(palette.textMain));
    send(message(Scintilla::Message::SetSelBack), 1, scintillaColor(palette.accentMuted));
    send(message(Scintilla::Message::SetCaretFore), scintillaColor(palette.accent));
    send(message(Scintilla::Message::SetCaretLineVisible), 1);
    send(message(Scintilla::Message::SetCaretLineBack), scintillaColor(palette.accentSubtle));
    send(message(Scintilla::Message::SetCaretLineBackAlpha), palette.dark ? 36 : 52);
    send(message(Scintilla::Message::SetWhitespaceFore), 1, scintillaColor(palette.borderStrong));
    send(message(Scintilla::Message::SetFoldMarginColour), 1, scintillaColor(palette.panelSubtle));
    send(message(Scintilla::Message::SetFoldMarginHiColour), 1,
         scintillaColor(palette.panelSubtle));

    applyLexerTheme(palette);
    send(message(Scintilla::Message::Colourise), 0, -1);
    themePending_ = false;
}

void EditorWidget::undoEdit() { send(message(Scintilla::Message::Undo)); }

void EditorWidget::redoEdit() { send(message(Scintilla::Message::Redo)); }

void EditorWidget::cutSelection() { send(message(Scintilla::Message::Cut)); }

void EditorWidget::copySelection() { send(message(Scintilla::Message::Copy)); }

void EditorWidget::pasteClipboard() { send(message(Scintilla::Message::Paste)); }

void EditorWidget::deleteSelection() { send(message(Scintilla::Message::Clear)); }

void EditorWidget::selectAllText() { send(message(Scintilla::Message::SelectAll)); }

FindResult EditorWidget::findText(const QString& query, const bool backwards, const bool matchCase,
                                  const bool wholeWord) {
    const QByteArray needle = query.toUtf8();
    if (needle.isEmpty()) {
        return {};
    }

    setSearchOptions(matchCase, wholeWord);
    const auto documentLength = send(message(Scintilla::Message::GetLength));
    const auto selectionStart = send(message(Scintilla::Message::GetSelectionStart));
    const auto selectionEnd = send(message(Scintilla::Message::GetSelectionEnd));

    const auto searchRange = [this, &needle](const sptr_t start, const sptr_t end) {
        send(message(Scintilla::Message::SetTargetStart), static_cast<uptr_t>(start));
        send(message(Scintilla::Message::SetTargetEnd), static_cast<uptr_t>(end));
        return sends(message(Scintilla::Message::SearchInTarget),
                     static_cast<uptr_t>(needle.size()), needle.constData());
    };

    sptr_t match =
        backwards ? searchRange(selectionStart, 0) : searchRange(selectionEnd, documentLength);
    bool wrapped = false;
    if (match < 0) {
        match =
            backwards ? searchRange(documentLength, selectionEnd) : searchRange(0, selectionStart);
        wrapped = match >= 0;
    }
    if (match < 0) {
        return {};
    }

    const auto matchStart = send(message(Scintilla::Message::GetTargetStart));
    const auto matchEnd = send(message(Scintilla::Message::GetTargetEnd));
    send(message(Scintilla::Message::SetSel), static_cast<uptr_t>(matchStart), matchEnd);
    send(message(Scintilla::Message::ScrollCaret));
    return {.found = true, .wrapped = wrapped};
}

bool EditorWidget::replaceSelection(const QString& query, const QString& replacement,
                                    const bool matchCase, const bool wholeWord) {
    const QByteArray needle = query.toUtf8();
    if (needle.isEmpty() || !selectionMatches(needle, matchCase, wholeWord)) {
        return false;
    }

    const QByteArray replacementBytes = replacement.toUtf8();
    sends(message(Scintilla::Message::ReplaceSel), 0, replacementBytes.constData());
    return true;
}

int EditorWidget::replaceAll(const QString& query, const QString& replacement, const bool matchCase,
                             const bool wholeWord) {
    const QByteArray needle = query.toUtf8();
    if (needle.isEmpty()) {
        return 0;
    }
    const QByteArray replacementBytes = replacement.toUtf8();
    setSearchOptions(matchCase, wholeWord);

    int replacements = 0;
    sptr_t searchStart = 0;
    sptr_t documentLength = send(message(Scintilla::Message::GetLength));
    send(message(Scintilla::Message::BeginUndoAction));
    while (searchStart <= documentLength) {
        send(message(Scintilla::Message::SetTargetStart), static_cast<uptr_t>(searchStart));
        send(message(Scintilla::Message::SetTargetEnd), static_cast<uptr_t>(documentLength));
        const auto match = sends(message(Scintilla::Message::SearchInTarget),
                                 static_cast<uptr_t>(needle.size()), needle.constData());
        if (match < 0) {
            break;
        }

        const auto matchStart = send(message(Scintilla::Message::GetTargetStart));
        sends(message(Scintilla::Message::ReplaceTarget),
              static_cast<uptr_t>(replacementBytes.size()), replacementBytes.constData());
        searchStart = matchStart + replacementBytes.size();
        documentLength = send(message(Scintilla::Message::GetLength));
        ++replacements;
    }
    send(message(Scintilla::Message::EndUndoAction));
    return replacements;
}

void EditorWidget::configureEditor() {
    send(message(Scintilla::Message::SetCodePage), 65001);
    send(message(Scintilla::Message::SetTabWidth), 4);
    send(message(Scintilla::Message::SetUseTabs), 0);
    send(message(Scintilla::Message::SetMargins), 1);
    send(message(Scintilla::Message::SetMarginTypeN), 0,
         static_cast<uptr_t>(Scintilla::MarginType::Number));
    send(message(Scintilla::Message::SetMarginMaskN), 0, 0);
    send(message(Scintilla::Message::SetMarginSensitiveN), 0, 0);
    updateLineNumberMarginWidth();

    send(message(Scintilla::Message::SetCaretWidth), 2);
    send(message(Scintilla::Message::SetScrollWidth), 1);
    send(message(Scintilla::Message::SetScrollWidthTracking), 1);
    send(message(Scintilla::Message::SetEndAtLastLine), 0);
}

void EditorWidget::updateLineNumberMarginWidth() {
    const auto lineCount = qMax<sptr_t>(1, send(message(Scintilla::Message::GetLineCount)));
    int digits = 1;
    for (sptr_t threshold = 10;
         lineCount >= threshold && threshold <= std::numeric_limits<sptr_t>::max() / 10;
         threshold *= 10) {
        ++digits;
    }
    if (digits == lineNumberDigits_) {
        return;
    }

    lineNumberDigits_ = digits;
    const QByteArray sample(digits, '9');
    const auto numberWidth =
        sends(message(Scintilla::Message::TextWidth), STYLE_LINENUMBER, sample.constData());
    constexpr sptr_t horizontalPadding = 16;
    send(message(Scintilla::Message::SetMarginWidthN), 0,
         qMax<sptr_t>(20, numberWidth + horizontalPadding));
}

void EditorWidget::updateLargeFileMode(const qsizetype contentSize) {
    const bool shouldUseLargeFileMode = contentSize >= largeFileThresholdBytes;
    if (largeFileMode_ == shouldUseLargeFileMode) {
        return;
    }

    largeFileMode_ = shouldUseLargeFileMode;
    if (largeFileMode_) {
        send(message(Scintilla::Message::EmptyUndoBuffer));
        send(message(Scintilla::Message::SetUndoCollection), 0);
    } else {
        send(message(Scintilla::Message::SetUndoCollection), 1);
    }
}

void EditorWidget::restoreViewState() {
    const auto length = send(message(Scintilla::Message::GetLength));
    const auto caret = qBound<sptr_t>(0, hibernatedViewState_.caret, length);
    const auto anchor = qBound<sptr_t>(0, hibernatedViewState_.anchor, length);
    send(message(Scintilla::Message::SetSel), static_cast<uptr_t>(anchor), caret);
    send(message(Scintilla::Message::SetFirstVisibleLine),
         static_cast<uptr_t>(qMax<sptr_t>(0, hibernatedViewState_.firstVisibleLine)));
    send(message(Scintilla::Message::SetXOffset),
         static_cast<uptr_t>(qMax<sptr_t>(0, hibernatedViewState_.horizontalOffset)));
}

void EditorWidget::setSearchOptions(const bool matchCase, const bool wholeWord) {
    int options = static_cast<int>(Scintilla::FindOption::None);
    if (matchCase) {
        options |= static_cast<int>(Scintilla::FindOption::MatchCase);
    }
    if (wholeWord) {
        options |= static_cast<int>(Scintilla::FindOption::WholeWord);
    }
    send(message(Scintilla::Message::SetSearchFlags), static_cast<uptr_t>(options));
}

bool EditorWidget::selectionMatches(const QByteArray& query, const bool matchCase,
                                    const bool wholeWord) {
    const auto selectionStart = send(message(Scintilla::Message::GetSelectionStart));
    const auto selectionEnd = send(message(Scintilla::Message::GetSelectionEnd));
    if (selectionStart == selectionEnd) {
        return false;
    }

    setSearchOptions(matchCase, wholeWord);
    send(message(Scintilla::Message::SetTargetStart), static_cast<uptr_t>(selectionStart));
    send(message(Scintilla::Message::SetTargetEnd), static_cast<uptr_t>(selectionEnd));
    const auto match = sends(message(Scintilla::Message::SearchInTarget),
                             static_cast<uptr_t>(query.size()), query.constData());
    return match == selectionStart &&
           send(message(Scintilla::Message::GetTargetEnd)) == selectionEnd;
}

void EditorWidget::applyLexerTheme(const ThemePalette& palette) {
    if (lexerName_ == QStringLiteral("cpp")) {
        setStyles(*this,
                  {SCE_C_COMMENT, SCE_C_COMMENTLINE, SCE_C_COMMENTDOC, SCE_C_COMMENTLINEDOC,
                   SCE_C_PREPROCESSORCOMMENT, SCE_C_PREPROCESSORCOMMENTDOC},
                  palette.textMuted, false, true);
        applyStyle(*this, SCE_C_NUMBER, palette.info);
        applyStyle(*this, SCE_C_WORD, palette.accent, true);
        setStyles(*this,
                  {SCE_C_STRING, SCE_C_CHARACTER, SCE_C_STRINGEOL, SCE_C_VERBATIM, SCE_C_REGEX,
                   SCE_C_STRINGRAW, SCE_C_TRIPLEVERBATIM, SCE_C_HASHQUOTEDSTRING, SCE_C_USERLITERAL,
                   SCE_C_ESCAPESEQUENCE},
                  palette.positive);
        applyStyle(*this, SCE_C_PREPROCESSOR, palette.warning);
        applyStyle(*this, SCE_C_WORD2, palette.info, true);
        applyStyle(*this, SCE_C_COMMENTDOCKEYWORD, palette.accent);
        applyStyle(*this, SCE_C_COMMENTDOCKEYWORDERROR, palette.danger);
        applyStyle(*this, SCE_C_GLOBALCLASS, palette.info);
    } else if (lexerName_ == QStringLiteral("python")) {
        setStyles(*this, {SCE_P_COMMENTLINE, SCE_P_COMMENTBLOCK}, palette.textMuted, false, true);
        applyStyle(*this, SCE_P_NUMBER, palette.info);
        setStyles(*this,
                  {SCE_P_STRING, SCE_P_CHARACTER, SCE_P_TRIPLE, SCE_P_TRIPLEDOUBLE, SCE_P_STRINGEOL,
                   SCE_P_FSTRING, SCE_P_FCHARACTER, SCE_P_FTRIPLE, SCE_P_FTRIPLEDOUBLE},
                  palette.positive);
        setStyles(*this, {SCE_P_WORD, SCE_P_WORD2}, palette.accent, true);
        setStyles(*this, {SCE_P_CLASSNAME, SCE_P_DEFNAME}, palette.info, true);
        applyStyle(*this, SCE_P_DECORATOR, palette.warning);
        applyStyle(*this, SCE_P_ATTRIBUTE, palette.info);
    } else if (lexerName_ == QStringLiteral("hypertext") ||
               lexerName_ == QStringLiteral("xml") || lexerName_ == QStringLiteral("phpscript")) {
        setStyles(*this, {SCE_H_TAG, SCE_H_TAGEND, SCE_H_XMLSTART, SCE_H_XMLEND}, palette.accent,
                  true);
        setStyles(*this, {SCE_H_ATTRIBUTE, SCE_H_ENTITY}, palette.info);
        setStyles(*this, {SCE_H_DOUBLESTRING, SCE_H_SINGLESTRING, SCE_H_VALUE}, palette.positive);
        setStyles(*this, {SCE_H_COMMENT, SCE_H_XCCOMMENT, SCE_H_SGML_COMMENT}, palette.textMuted,
                  false, true);
        applyStyle(*this, SCE_H_NUMBER, palette.warning);
        setStyles(*this, {SCE_H_TAGUNKNOWN, SCE_H_ATTRIBUTEUNKNOWN, SCE_H_SGML_ERROR},
                  palette.danger);
        setStyles(*this, {SCE_HJ_COMMENT, SCE_HJ_COMMENTLINE, SCE_HJ_COMMENTDOC},
                  palette.textMuted, false, true);
        applyStyle(*this, SCE_HJ_NUMBER, palette.info);
        setStyles(*this, {SCE_HJ_WORD, SCE_HJ_KEYWORD}, palette.accent, true);
        setStyles(*this,
                  {SCE_HJ_DOUBLESTRING, SCE_HJ_SINGLESTRING, SCE_HJ_REGEX,
                   SCE_HJ_TEMPLATELITERAL},
                  palette.positive);
        setStyles(*this, {SCE_HPHP_COMMENT, SCE_HPHP_COMMENTLINE}, palette.textMuted, false, true);
        applyStyle(*this, SCE_HPHP_WORD, palette.accent, true);
        applyStyle(*this, SCE_HPHP_NUMBER, palette.info);
        setStyles(*this,
                  {SCE_HPHP_HSTRING, SCE_HPHP_SIMPLESTRING, SCE_HPHP_HSTRING_VARIABLE},
                  palette.positive);
        setStyles(*this, {SCE_HPHP_VARIABLE, SCE_HPHP_COMPLEX_VARIABLE}, palette.warning);
    } else if (lexerName_ == QStringLiteral("json")) {
        applyStyle(*this, SCE_JSON_NUMBER, palette.info);
        setStyles(*this, {SCE_JSON_STRING, SCE_JSON_URI, SCE_JSON_COMPACTIRI}, palette.positive);
        applyStyle(*this, SCE_JSON_PROPERTYNAME, palette.accent);
        setStyles(*this, {SCE_JSON_LINECOMMENT, SCE_JSON_BLOCKCOMMENT}, palette.textMuted, false,
                  true);
        setStyles(*this, {SCE_JSON_KEYWORD, SCE_JSON_LDKEYWORD}, palette.warning, true);
        setStyles(*this, {SCE_JSON_STRINGEOL, SCE_JSON_ERROR}, palette.danger);
    } else if (lexerName_ == QStringLiteral("markdown")) {
        setStyles(*this,
                  {SCE_MARKDOWN_HEADER1, SCE_MARKDOWN_HEADER2, SCE_MARKDOWN_HEADER3,
                   SCE_MARKDOWN_HEADER4, SCE_MARKDOWN_HEADER5, SCE_MARKDOWN_HEADER6},
                  palette.accent, true);
        setStyles(*this, {SCE_MARKDOWN_STRONG1, SCE_MARKDOWN_STRONG2}, palette.textMain, true);
        setStyles(*this, {SCE_MARKDOWN_EM1, SCE_MARKDOWN_EM2}, palette.textSecondary, false, true);
        setStyles(*this, {SCE_MARKDOWN_CODE, SCE_MARKDOWN_CODE2, SCE_MARKDOWN_CODEBK},
                  palette.positive);
        applyStyle(*this, SCE_MARKDOWN_LINK, palette.info);
        applyStyle(*this, SCE_MARKDOWN_BLOCKQUOTE, palette.textMuted, false, true);
        setStyles(*this, {SCE_MARKDOWN_ULIST_ITEM, SCE_MARKDOWN_OLIST_ITEM}, palette.warning);
    } else if (lexerName_ == QStringLiteral("rust")) {
        setStyles(*this,
                  {SCE_RUST_COMMENTBLOCK, SCE_RUST_COMMENTLINE, SCE_RUST_COMMENTBLOCKDOC,
                   SCE_RUST_COMMENTLINEDOC},
                  palette.textMuted, false, true);
        applyStyle(*this, SCE_RUST_NUMBER, palette.info);
        setStyles(*this,
                  {SCE_RUST_WORD, SCE_RUST_WORD2, SCE_RUST_WORD3, SCE_RUST_WORD4, SCE_RUST_WORD5,
                   SCE_RUST_WORD6, SCE_RUST_WORD7},
                  palette.accent, true);
        setStyles(*this,
                  {SCE_RUST_STRING, SCE_RUST_STRINGR, SCE_RUST_CHARACTER, SCE_RUST_BYTESTRING,
                   SCE_RUST_BYTESTRINGR, SCE_RUST_BYTECHARACTER, SCE_RUST_CSTRING,
                   SCE_RUST_CSTRINGR},
                  palette.positive);
        setStyles(*this, {SCE_RUST_LIFETIME, SCE_RUST_MACRO}, palette.warning);
        applyStyle(*this, SCE_RUST_LEXERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("css")) {
        applyStyle(*this, SCE_CSS_COMMENT, palette.textMuted, false, true);
        setStyles(*this, {SCE_CSS_TAG, SCE_CSS_CLASS, SCE_CSS_ID, SCE_CSS_ATTRIBUTE},
                  palette.accent);
        setStyles(*this,
                  {SCE_CSS_PSEUDOCLASS, SCE_CSS_PSEUDOELEMENT, SCE_CSS_EXTENDED_PSEUDOCLASS,
                   SCE_CSS_EXTENDED_PSEUDOELEMENT},
                  palette.info);
        setStyles(*this,
                  {SCE_CSS_IDENTIFIER, SCE_CSS_IDENTIFIER2, SCE_CSS_IDENTIFIER3,
                   SCE_CSS_EXTENDED_IDENTIFIER, SCE_CSS_VARIABLE},
                  palette.textSecondary);
        setStyles(*this, {SCE_CSS_DOUBLESTRING, SCE_CSS_SINGLESTRING}, palette.positive);
        setStyles(*this, {SCE_CSS_IMPORTANT, SCE_CSS_DIRECTIVE, SCE_CSS_GROUP_RULE},
                  palette.warning, true);
        setStyles(*this, {SCE_CSS_UNKNOWN_IDENTIFIER, SCE_CSS_UNKNOWN_PSEUDOCLASS},
                  palette.danger);
    } else if (lexerName_ == QStringLiteral("bash")) {
        applyStyle(*this, SCE_SH_COMMENTLINE, palette.textMuted, false, true);
        applyStyle(*this, SCE_SH_WORD, palette.accent, true);
        applyStyle(*this, SCE_SH_NUMBER, palette.info);
        setStyles(*this,
                  {SCE_SH_STRING, SCE_SH_CHARACTER, SCE_SH_BACKTICKS, SCE_SH_HERE_DELIM,
                   SCE_SH_HERE_Q},
                  palette.positive);
        setStyles(*this, {SCE_SH_SCALAR, SCE_SH_PARAM}, palette.warning);
        applyStyle(*this, SCE_SH_ERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("yaml")) {
        applyStyle(*this, SCE_YAML_COMMENT, palette.textMuted, false, true);
        applyStyle(*this, SCE_YAML_IDENTIFIER, palette.accent);
        setStyles(*this, {SCE_YAML_DEFAULT, SCE_YAML_TEXT}, palette.warning);
        applyStyle(*this, SCE_YAML_KEYWORD, palette.warning, true);
        applyStyle(*this, SCE_YAML_NUMBER, palette.warning);
        setStyles(*this, {SCE_YAML_REFERENCE, SCE_YAML_DOCUMENT}, palette.positive);
        applyStyle(*this, SCE_YAML_ERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("toml")) {
        applyStyle(*this, SCE_TOML_COMMENT, palette.textMuted, false, true);
        setStyles(*this, {SCE_TOML_IDENTIFIER, SCE_TOML_KEY, SCE_TOML_TABLE}, palette.accent);
        applyStyle(*this, SCE_TOML_KEYWORD, palette.warning, true);
        setStyles(*this, {SCE_TOML_NUMBER, SCE_TOML_DATETIME}, palette.info);
        setStyles(*this,
                  {SCE_TOML_STRING_SQ, SCE_TOML_STRING_DQ, SCE_TOML_TRIPLE_STRING_SQ,
                   SCE_TOML_TRIPLE_STRING_DQ, SCE_TOML_ESCAPECHAR},
                  palette.positive);
        setStyles(*this, {SCE_TOML_ERROR, SCE_TOML_STRINGEOL}, palette.danger);
    } else if (lexerName_ == QStringLiteral("sql")) {
        setStyles(*this,
                  {SCE_SQL_COMMENT, SCE_SQL_COMMENTLINE, SCE_SQL_COMMENTDOC,
                   SCE_SQL_COMMENTLINEDOC},
                  palette.textMuted, false, true);
        applyStyle(*this, SCE_SQL_NUMBER, palette.info);
        setStyles(*this, {SCE_SQL_WORD, SCE_SQL_WORD2}, palette.accent, true);
        setStyles(*this, {SCE_SQL_STRING, SCE_SQL_CHARACTER}, palette.positive);
        applyStyle(*this, SCE_SQL_QUOTEDIDENTIFIER, palette.warning);
        applyStyle(*this, SCE_SQL_COMMENTDOCKEYWORDERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("ruby")) {
        setStyles(*this, {SCE_RB_COMMENTLINE, SCE_RB_POD}, palette.textMuted, false, true);
        applyStyle(*this, SCE_RB_WORD, palette.accent, true);
        applyStyle(*this, SCE_RB_NUMBER, palette.info);
        setStyles(*this,
                  {SCE_RB_STRING, SCE_RB_CHARACTER, SCE_RB_REGEX, SCE_RB_SYMBOL,
                   SCE_RB_BACKTICKS, SCE_RB_HERE_Q, SCE_RB_HERE_QQ, SCE_RB_HERE_QX},
                  palette.positive);
        setStyles(*this, {SCE_RB_CLASSNAME, SCE_RB_DEFNAME, SCE_RB_MODULE_NAME}, palette.info,
                  true);
        setStyles(*this, {SCE_RB_GLOBAL, SCE_RB_INSTANCE_VAR, SCE_RB_CLASS_VAR},
                  palette.warning);
        applyStyle(*this, SCE_RB_ERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("lua")) {
        setStyles(*this, {SCE_LUA_COMMENT, SCE_LUA_COMMENTLINE, SCE_LUA_COMMENTDOC},
                  palette.textMuted, false, true);
        applyStyle(*this, SCE_LUA_WORD, palette.accent, true);
        applyStyle(*this, SCE_LUA_NUMBER, palette.info);
        setStyles(*this,
                  {SCE_LUA_STRING, SCE_LUA_CHARACTER, SCE_LUA_LITERALSTRING}, palette.positive);
        setStyles(*this, {SCE_LUA_WORD2, SCE_LUA_WORD3, SCE_LUA_WORD4}, palette.info);
        applyStyle(*this, SCE_LUA_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("dart")) {
        setStyles(*this,
                  {SCE_DART_COMMENTLINE, SCE_DART_COMMENTLINEDOC, SCE_DART_COMMENTBLOCK,
                   SCE_DART_COMMENTBLOCKDOC},
                  palette.textMuted, false, true);
        setStyles(*this,
                  {SCE_DART_KW_PRIMARY, SCE_DART_KW_SECONDARY, SCE_DART_KW_TERTIARY,
                   SCE_DART_KW_TYPE},
                  palette.accent, true);
        applyStyle(*this, SCE_DART_NUMBER, palette.info);
        setStyles(*this,
                  {SCE_DART_STRING_SQ, SCE_DART_STRING_DQ, SCE_DART_TRIPLE_STRING_SQ,
                   SCE_DART_TRIPLE_STRING_DQ, SCE_DART_RAWSTRING_SQ, SCE_DART_RAWSTRING_DQ,
                   SCE_DART_ESCAPECHAR},
                  palette.positive);
        applyStyle(*this, SCE_DART_METADATA, palette.warning);
        applyStyle(*this, SCE_DART_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("zig")) {
        setStyles(*this,
                  {SCE_ZIG_COMMENTLINE, SCE_ZIG_COMMENTLINEDOC, SCE_ZIG_COMMENTLINETOP},
                  palette.textMuted, false, true);
        setStyles(*this,
                  {SCE_ZIG_KW_PRIMARY, SCE_ZIG_KW_SECONDARY, SCE_ZIG_KW_TERTIARY,
                   SCE_ZIG_KW_TYPE},
                  palette.accent, true);
        applyStyle(*this, SCE_ZIG_NUMBER, palette.info);
        setStyles(*this,
                  {SCE_ZIG_CHARACTER, SCE_ZIG_STRING, SCE_ZIG_MULTISTRING, SCE_ZIG_ESCAPECHAR},
                  palette.positive);
        setStyles(*this, {SCE_ZIG_FUNCTION, SCE_ZIG_BUILTIN_FUNCTION}, palette.info);
        applyStyle(*this, SCE_ZIG_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("props")) {
        applyStyle(*this, SCE_PROPS_COMMENT, palette.textMuted, false, true);
        applyStyle(*this, SCE_PROPS_SECTION, palette.accent, true);
        applyStyle(*this, SCE_PROPS_KEY, palette.info);
        applyStyle(*this, SCE_PROPS_ASSIGNMENT, palette.warning);
        applyStyle(*this, SCE_PROPS_DEFVAL, palette.positive);
    } else if (lexerName_ == QStringLiteral("diff")) {
        setStyles(*this, {SCE_DIFF_COMMENT, SCE_DIFF_COMMAND}, palette.textMuted);
        setStyles(*this, {SCE_DIFF_HEADER, SCE_DIFF_POSITION}, palette.accent, true);
        setStyles(*this, {SCE_DIFF_ADDED, SCE_DIFF_PATCH_ADD}, palette.positive);
        setStyles(*this,
                  {SCE_DIFF_DELETED, SCE_DIFF_PATCH_DELETE, SCE_DIFF_REMOVED_PATCH_ADD,
                   SCE_DIFF_REMOVED_PATCH_DELETE},
                  palette.danger);
        applyStyle(*this, SCE_DIFF_CHANGED, palette.warning);
    } else if (lexerName_ == QStringLiteral("makefile")) {
        applyStyle(*this, SCE_MAKE_COMMENT, palette.textMuted, false, true);
        applyStyle(*this, SCE_MAKE_PREPROCESSOR, palette.warning);
        applyStyle(*this, SCE_MAKE_IDENTIFIER, palette.info);
        applyStyle(*this, SCE_MAKE_TARGET, palette.accent, true);
        applyStyle(*this, SCE_MAKE_IDEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("cmake")) {
        applyStyle(*this, SCE_CMAKE_COMMENT, palette.textMuted, false, true);
        setStyles(*this,
                  {SCE_CMAKE_STRINGDQ, SCE_CMAKE_STRINGLQ, SCE_CMAKE_STRINGRQ,
                   SCE_CMAKE_STRINGVAR},
                  palette.positive);
        setStyles(*this, {SCE_CMAKE_COMMANDS, SCE_CMAKE_WHILEDEF, SCE_CMAKE_FOREACHDEF,
                          SCE_CMAKE_IFDEFINEDEF, SCE_CMAKE_MACRODEF},
                  palette.accent, true);
        applyStyle(*this, SCE_CMAKE_PARAMETERS, palette.info);
        applyStyle(*this, SCE_CMAKE_VARIABLE, palette.warning);
        applyStyle(*this, SCE_CMAKE_NUMBER, palette.info);
    }
}

} // namespace ketplus

#include "editor/EditorWidget.h"

#include "editor/SyntaxDefinition.h"
#include "ui/Theme.h"

#include <ILexer.h>
#include <Lexilla.h>
#include <SciLexer.h>
#include <Scintilla.h>
#include <ScintillaMessages.h>

#include <QByteArrayList>
#include <QColor>
#include <QContextMenuEvent>
#include <QEvent>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QSet>
#include <QStringList>
#include <QtMath>

#include <algorithm>
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

constexpr uptr_t bookmarkMargin = 1;
constexpr uptr_t foldMargin = 2;
constexpr int bookmarkMarker = 24;
constexpr int bookmarkMask = 1 << bookmarkMarker;
constexpr uptr_t findIndicator = INDICATOR_CONTAINER;
constexpr uptr_t selectionIndicator = INDICATOR_CONTAINER + 1;
constexpr uptr_t embeddedStyleIndicator = INDICATOR_CONTAINER + 2;
constexpr uptr_t linkIndicator = INDICATOR_CONTAINER + 3;
constexpr sptr_t symbolMarginWidth = 14;
// How far back a <style> block may open and still color the visible CSS.
constexpr sptr_t embeddedStyleLookBehind = 256 * 1024;

constexpr uptr_t marker(const Scintilla::MarkerOutline value) { return static_cast<uptr_t>(value); }

// Draws a thin, antialiased chevron into Scintilla's RGBA pixel buffer.
QByteArray chevronPixels(const int size, const int scalePercent, const QString& color,
                         const bool open) {
    const qreal scale = scalePercent / 100.0;
    const int pixels = qMax(1, qRound(size * scale));
    QImage image(pixels, pixels, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.scale(scale, scale);
        QPen pen(QColor(color), 1.5);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter.setPen(pen);
        const qreal middle = size / 2.0;
        const qreal arm = size * 0.23;
        if (open) {
            painter.drawPolyline(QPolygonF{QPointF(middle - arm * 1.4, middle - arm * 0.7),
                                           QPointF(middle, middle + arm * 0.7),
                                           QPointF(middle + arm * 1.4, middle - arm * 0.7)});
        } else {
            painter.drawPolyline(QPolygonF{QPointF(middle - arm * 0.7, middle - arm * 1.4),
                                           QPointF(middle + arm * 0.7, middle),
                                           QPointF(middle - arm * 0.7, middle + arm * 1.4)});
        }
    }

    // Scintilla reads tightly packed RGBA rows, so scanline padding is dropped here.
    const QImage rgba = image.convertToFormat(QImage::Format_RGBA8888);
    QByteArray buffer;
    buffer.reserve(static_cast<qsizetype>(pixels) * pixels * 4);
    for (int row = 0; row < pixels; ++row) {
        buffer.append(reinterpret_cast<const char*>(rgba.constScanLine(row)),
                      static_cast<qsizetype>(pixels) * 4);
    }
    return buffer;
}

bool isCssNameCharacter(const char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '-' || character == '_' ||
           static_cast<unsigned char>(character) >= 0x80;
}

bool isDigit(const char character) { return character >= '0' && character <= '9'; }

bool isWordCharacter(const int character) {
    // Scintilla returns UTF-8 continuation bytes as negative values.
    return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') || character == '_' || character < 0 ||
           character >= 0x80;
}

bool isBrace(const int character) {
    return character == '(' || character == ')' || character == '[' || character == ']' ||
           character == '{' || character == '}';
}

bool isHorizontalSpace(const char character) { return character == ' ' || character == '\t'; }

void trimTrailingSpaces(QByteArray& line) {
    qsizetype end = line.size();
    while (end > 0 && isHorizontalSpace(line.at(end - 1))) {
        --end;
    }
    line.truncate(end);
}

struct CommentTokens final {
    QByteArray open;
    QByteArray close;
};

CommentTokens commentTokensForSyntax(const QString& syntax) {
    static const QStringList hashSyntaxes{
        QStringLiteral("python"),       QStringLiteral("shell"),    QStringLiteral("dockerfile"),
        QStringLiteral("yaml"),         QStringLiteral("toml"),     QStringLiteral("ruby"),
        QStringLiteral("cmake"),        QStringLiteral("makefile"), QStringLiteral("properties"),
        QStringLiteral("perl"),         QStringLiteral("r"),        QStringLiteral("powershell"),
        QStringLiteral("tcl"),          QStringLiteral("julia"),    QStringLiteral("nim"),
        QStringLiteral("coffeescript"), QStringLiteral("gdscript")};
    if (syntax == QStringLiteral("haskell") || syntax == QStringLiteral("vhdl")) {
        return {QByteArrayLiteral("--"), {}};
    }
    if (syntax == QStringLiteral("erlang") || syntax == QStringLiteral("latex")) {
        return {QByteArrayLiteral("%"), {}};
    }
    if (syntax == QStringLiteral("lisp") || syntax == QStringLiteral("assembly")) {
        return {QByteArrayLiteral(";"), {}};
    }
    if (syntax == QStringLiteral("batch")) {
        return {QByteArrayLiteral("::"), {}};
    }
    if (syntax == QStringLiteral("visual-basic")) {
        return {QByteArrayLiteral("'"), {}};
    }
    if (syntax == QStringLiteral("fortran")) {
        return {QByteArrayLiteral("!"), {}};
    }
    if (syntax == QStringLiteral("ocaml")) {
        return {QByteArrayLiteral("(*"), QByteArrayLiteral("*)")};
    }
    if (syntax == QStringLiteral("scss") || syntax == QStringLiteral("less")) {
        return {QByteArrayLiteral("/*"), QByteArrayLiteral("*/")};
    }
    static const QStringList markupSyntaxes{QStringLiteral("html"), QStringLiteral("xml"),
                                            QStringLiteral("markdown")};
    static const QStringList uncommentableSyntaxes{QString(), QStringLiteral("plain"),
                                                   QStringLiteral("json"), QStringLiteral("diff"),
                                                   QStringLiteral("large-file")};

    if (hashSyntaxes.contains(syntax)) {
        return {QByteArrayLiteral("#"), {}};
    }
    if (syntax == QStringLiteral("sql") || syntax == QStringLiteral("lua")) {
        return {QByteArrayLiteral("--"), {}};
    }
    if (markupSyntaxes.contains(syntax)) {
        return {QByteArrayLiteral("<!--"), QByteArrayLiteral("-->")};
    }
    if (syntax == QStringLiteral("css")) {
        return {QByteArrayLiteral("/*"), QByteArrayLiteral("*/")};
    }
    if (uncommentableSyntaxes.contains(syntax)) {
        return {};
    }
    return {QByteArrayLiteral("//"), {}};
}

} // namespace

EditorWidget::EditorWidget(QWidget* parent)
    : ScintillaEditBase(parent), document_(std::make_unique<Document>()) {
    configureEditor();
    viewport()->setMouseTracking(true);
    viewport()->installEventFilter(this);
    connect(this, &ScintillaEditBase::savePointChanged, this, [this](const bool dirty) {
        if (internalMutation_) {
            return;
        }
        document_->setModified(dirty);
        emit dirtyStateChanged(dirty);
    });
    connect(this, &ScintillaEditBase::updateUi, this, [this](const Scintilla::Update updated) {
        const auto position = send(message(Scintilla::Message::GetCurrentPos));
        const auto line = send(message(Scintilla::Message::LineFromPosition), position);
        const auto column = send(message(Scintilla::Message::GetColumn), position);
        emit cursorPositionChanged(static_cast<int>(line + 1), static_cast<int>(column + 1));
        updateBraceHighlight();
        constexpr int selectionMatchTriggers = static_cast<int>(Scintilla::Update::Content) |
                                               static_cast<int>(Scintilla::Update::Selection) |
                                               static_cast<int>(Scintilla::Update::VScroll);
        if ((static_cast<int>(updated) & selectionMatchTriggers) != 0) {
            highlightSelectionMatches();
        }
        constexpr int embeddedStyleTriggers = static_cast<int>(Scintilla::Update::Content) |
                                              static_cast<int>(Scintilla::Update::VScroll);
        if ((static_cast<int>(updated) & embeddedStyleTriggers) != 0) {
            updateEmbeddedStyleHighlight();
        }
        emit editorStateChanged();
    });
    connect(this, &ScintillaEditBase::modified, this,
            [this](const Scintilla::ModificationFlags type, Scintilla::Position,
                   Scintilla::Position, Scintilla::Position, const QByteArray&, Scintilla::Position,
                   Scintilla::FoldLevel, Scintilla::FoldLevel) {
                constexpr int textChanges =
                    static_cast<int>(Scintilla::ModificationFlags::InsertText) |
                    static_cast<int>(Scintilla::ModificationFlags::DeleteText);
                if ((static_cast<int>(type) & textChanges) != 0) {
                    embeddedStyleDirty_ = true;
                }
            });
    connect(this, &ScintillaEditBase::linesAdded, this,
            [this](Scintilla::Position) { updateLineNumberMarginWidth(); });
    connect(this, &ScintillaEditBase::charAdded, this, &EditorWidget::handleCharAdded);
    connect(this, &ScintillaEditBase::marginClicked, this,
            [this](const Scintilla::Position position, Scintilla::KeyMod, const int margin) {
                if (margin == static_cast<int>(bookmarkMargin)) {
                    toggleBookmarkAtLine(send(message(Scintilla::Message::LineFromPosition),
                                              static_cast<uptr_t>(position)));
                }
            });
}

void EditorWidget::contextMenuEvent(QContextMenuEvent* event) {
    const auto position =
        send(message(Scintilla::Message::PositionFromPointClose),
             static_cast<uptr_t>(event->pos().x()), static_cast<sptr_t>(event->pos().y()));
    const auto selectionStart = send(message(Scintilla::Message::GetSelectionStart));
    const auto selectionEnd = send(message(Scintilla::Message::GetSelectionEnd));
    if (position >= 0 && (position < selectionStart || position > selectionEnd)) {
        send(message(Scintilla::Message::SetEmptySelection), static_cast<uptr_t>(position));
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
    // New line breaks follow the file's existing style instead of the platform default
    // (CRLF on Windows), so edits never mix line endings.
    const qsizetype firstLineFeed = text.indexOf('\n');
    const int endOfLineMode = firstLineFeed > 0 && text.at(firstLineFeed - 1) == '\r' ? SC_EOL_CRLF
                              : firstLineFeed < 0 && text.contains('\r')              ? SC_EOL_CR
                                                                                      : SC_EOL_LF;
    send(message(Scintilla::Message::SetEOLMode), static_cast<uptr_t>(endOfLineMode));
    sends(message(Scintilla::Message::SetText), 0, text.constData());
    send(message(Scintilla::Message::EmptyUndoBuffer));
    markSaved();
    hibernated_ = false;
    internalMutation_ = false;
    updateLineNumberMarginWidth();
    applyViewOptions();
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
    auto result = document_->load(path, document_->textEncoding());
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
    foldingEnabled_ = false;
    clearSearchScope();
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
    updateLargeFileMode(static_cast<qsizetype>(send(message(Scintilla::Message::GetLength))));
    if (largeFileMode_) {
        lexerName_ = QStringLiteral("null");
        syntaxName_ = QStringLiteral("large-file");
        Scintilla::ILexer5* lexer = CreateLexer("null");
        send(message(Scintilla::Message::SetILexer), 0, reinterpret_cast<sptr_t>(lexer));
        foldingEnabled_ = false;
        applyViewOptions();
        send(message(Scintilla::Message::Colourise), 0, -1);
        return;
    }

    const SyntaxDefinition* chosenDefinition =
        syntaxOverride_.isEmpty() ? nullptr : syntaxDefinitionByName(syntaxOverride_);
    const auto& definition =
        chosenDefinition != nullptr ? *chosenDefinition : syntaxDefinitionForPath(filePath);
    lexerName_ = QString::fromLatin1(definition.lexer);
    syntaxName_ = QString::fromLatin1(definition.name);
    Scintilla::ILexer5* lexer = CreateLexer(definition.lexer);
    send(message(Scintilla::Message::SetILexer), 0, reinterpret_cast<sptr_t>(lexer));
    foldingEnabled_ = false;
    for (std::size_t index = 0; index < definition.keywordSets.size(); ++index) {
        const char* keywords = definition.keywordSets[index];
        sends(message(Scintilla::Message::SetKeyWords), static_cast<uptr_t>(index),
              keywords == nullptr ? "" : keywords);
    }
    if (syntaxName_ == QStringLiteral("scss") || syntaxName_ == QStringLiteral("less")) {
        const QByteArray key = "lexer.css." + syntaxName_.toLatin1() + ".language";
        sends(message(Scintilla::Message::SetProperty), reinterpret_cast<uptr_t>(key.constData()),
              "1");
    }
    applyViewOptions();
    send(message(Scintilla::Message::Colourise), 0, -1);
    embeddedStyleDirty_ = true;
    updateEmbeddedStyleHighlight();
}

void EditorWidget::setEditorSettings(const EditorSettings& settings) {
    editorSettings_ = settings.normalized();
}

const EditorSettings& EditorWidget::editorSettings() const noexcept { return editorSettings_; }

void EditorWidget::setViewOptions(const EditorViewOptions& options) {
    viewOptions_ = options.normalized();
    if (applyViewOptions()) {
        send(message(Scintilla::Message::Colourise), 0, -1);
    }
}

const EditorViewOptions& EditorWidget::viewOptions() const noexcept { return viewOptions_; }

void EditorWidget::setSyntaxOverride(const QString& syntaxName) {
    syntaxOverride_ = syntaxDefinitionByName(syntaxName) != nullptr ? syntaxName : QString();
    configureLexerForPath(document_->filePath());
}

QString EditorWidget::syntaxOverride() const { return syntaxOverride_; }

LineEnding EditorWidget::lineEnding() const {
    switch (send(message(Scintilla::Message::GetEOLMode))) {
    case SC_EOL_CRLF:
        return LineEnding::CrLf;
    case SC_EOL_CR:
        return LineEnding::Cr;
    default:
        return LineEnding::Lf;
    }
}

void EditorWidget::setLineEnding(const LineEnding lineEnding) {
    const int mode = lineEnding == LineEnding::CrLf ? SC_EOL_CRLF
                     : lineEnding == LineEnding::Cr ? SC_EOL_CR
                                                    : SC_EOL_LF;
    send(message(Scintilla::Message::SetEOLMode), static_cast<uptr_t>(mode));
    send(message(Scintilla::Message::ConvertEOLs), static_cast<uptr_t>(mode));
}

Document::Result EditorWidget::reloadWithEncoding(const TextEncoding encoding) {
    if (document_->isUntitled()) {
        return {false, QStringLiteral("Save the file before reopening it with another encoding.")};
    }
    auto result = document_->load(document_->filePath(), encoding);
    if (!result.ok) {
        return {false, result.error};
    }
    setText(result.content);
    configureLexerForPath(document_->filePath());
    return {true, {}};
}

void EditorWidget::replaceAllText(const QByteArray& text) {
    send(message(Scintilla::Message::SetTargetStart), 0);
    send(message(Scintilla::Message::SetTargetEnd),
         static_cast<uptr_t>(send(message(Scintilla::Message::GetLength))));
    sends(message(Scintilla::Message::ReplaceTarget), static_cast<uptr_t>(text.size()),
          text.constData());
}

void EditorWidget::selectTextRange(const int line, const int column, const int length) {
    const sptr_t lastLine = send(message(Scintilla::Message::GetLineCount)) - 1;
    const sptr_t lineIndex = std::clamp<sptr_t>(line - 1, 0, lastLine);
    const auto lineStart =
        send(message(Scintilla::Message::PositionFromLine), static_cast<uptr_t>(lineIndex));
    const QString lineText =
        QString::fromUtf8(textRange(lineStart, send(message(Scintilla::Message::GetLineEndPosition),
                                                    static_cast<uptr_t>(lineIndex))));
    // Search results count UTF-16 characters; Scintilla positions are UTF-8 bytes.
    const auto from = lineStart + lineText.left(qMax(0, column)).toUtf8().size();
    const auto to = lineStart + lineText.left(qMax(0, column + length)).toUtf8().size();
    send(message(Scintilla::Message::EnsureVisibleEnforcePolicy), static_cast<uptr_t>(lineIndex));
    send(message(Scintilla::Message::SetSel), static_cast<uptr_t>(from), to);
    send(message(Scintilla::Message::ScrollCaret));
}

int EditorWidget::highlightSelectionMatches() {
    if (selectionMatchesActive_) {
        send(message(Scintilla::Message::SetIndicatorCurrent), selectionIndicator);
        send(message(Scintilla::Message::IndicatorClearRange), 0,
             send(message(Scintilla::Message::GetLength)));
        selectionMatchesActive_ = false;
    }
    if (!viewOptions_.highlightSelectionMatches || hibernated_ || selectionCount() != 1) {
        return 0;
    }

    const auto selectionStart = send(message(Scintilla::Message::GetSelectionStart));
    const auto selectionEnd = send(message(Scintilla::Message::GetSelectionEnd));
    constexpr sptr_t minimumWordLength = 2;
    constexpr sptr_t maximumWordLength = 200;
    if (selectionEnd - selectionStart < minimumWordLength ||
        selectionEnd - selectionStart > maximumWordLength) {
        return 0;
    }
    // Only a selection that is exactly one whole word lights up its other occurrences.
    if (send(message(Scintilla::Message::WordStartPosition), static_cast<uptr_t>(selectionStart),
             1) != selectionStart ||
        send(message(Scintilla::Message::WordEndPosition), static_cast<uptr_t>(selectionStart),
             1) != selectionEnd) {
        return 0;
    }

    const QByteArray word = textRange(selectionStart, selectionEnd);
    // Mark only the visible lines, so the cost does not grow with the file.
    const auto firstLine =
        send(message(Scintilla::Message::DocLineFromVisible),
             static_cast<uptr_t>(send(message(Scintilla::Message::GetFirstVisibleLine))));
    const auto lastLine = qMin(firstLine + send(message(Scintilla::Message::LinesOnScreen)) + 1,
                               send(message(Scintilla::Message::GetLineCount)) - 1);
    auto searchStart =
        send(message(Scintilla::Message::PositionFromLine), static_cast<uptr_t>(firstLine));
    const auto searchEnd =
        send(message(Scintilla::Message::GetLineEndPosition), static_cast<uptr_t>(lastLine));

    setSearchOptions(SearchOptions{.matchCase = true, .wholeWord = true});
    send(message(Scintilla::Message::SetIndicatorCurrent), selectionIndicator);
    int count = 0;
    while (searchStart < searchEnd && count < selectionMatchLimit) {
        send(message(Scintilla::Message::SetTargetStart), static_cast<uptr_t>(searchStart));
        send(message(Scintilla::Message::SetTargetEnd), static_cast<uptr_t>(searchEnd));
        if (sends(message(Scintilla::Message::SearchInTarget), static_cast<uptr_t>(word.size()),
                  word.constData()) < 0) {
            break;
        }
        const auto matchStart = send(message(Scintilla::Message::GetTargetStart));
        const auto matchEnd = send(message(Scintilla::Message::GetTargetEnd));
        if (matchStart != selectionStart) {
            send(message(Scintilla::Message::IndicatorFillRange), static_cast<uptr_t>(matchStart),
                 matchEnd - matchStart);
            ++count;
        }
        searchStart = matchEnd > matchStart ? matchEnd : matchStart + 1;
    }
    selectionMatchesActive_ = count > 0;
    return count;
}

void EditorWidget::shareDocumentWith(const EditorWidget& source) {
    const auto documentPointer = source.send(message(Scintilla::Message::GetDocPointer));
    if (send(message(Scintilla::Message::GetDocPointer)) != documentPointer) {
        // Scintilla reference-counts documents, so this view keeps the text alive until
        // it is destroyed. The lexer and styling belong to the document and stay shared.
        internalMutation_ = true;
        send(message(Scintilla::Message::SetDocPointer), 0, documentPointer);
        internalMutation_ = false;
    }
    lexerName_ = source.lexerName_;
    syntaxName_ = source.syntaxName_;
    largeFileMode_ = source.largeFileMode_;
    foldingEnabled_ = source.foldingEnabled_;
    highlightedBrace_ = -1;
    highlightedBraceMatch_ = -1;
    embeddedStyleDirty_ = true;
    lineNumberDigits_ = 0;
    updateLineNumberMarginWidth();
    applyViewOptions();
}

void EditorWidget::applyTheme(const ThemePalette& palette) {
    const auto family = editorSettings_.fontFamily.toUtf8();
    const int sizeHundredthPoints =
        qRound(editorSettings_.fontSizePixels * 72.0 * 100.0 / logicalDpiY());

    sends(message(Scintilla::Message::StyleSetFont), STYLE_DEFAULT, family.constData());
    send(message(Scintilla::Message::StyleSetSizeFractional), STYLE_DEFAULT, sizeHundredthPoints);
    send(message(Scintilla::Message::StyleSetFore), STYLE_DEFAULT,
         scintillaColor(palette.textMain));
    send(message(Scintilla::Message::StyleSetBack), STYLE_DEFAULT,
         scintillaColor(palette.panelBackground));
    send(message(Scintilla::Message::StyleClearAll));

    // Scintilla sizes fonts in points, so convert from logical pixels and add
    // balanced leading after measuring the selected font's native line height.
    send(message(Scintilla::Message::SetExtraAscent), 0);
    send(message(Scintilla::Message::SetExtraDescent), 0);
    const int nativeLineHeight = static_cast<int>(send(message(Scintilla::Message::TextHeight), 0));
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
    applyStyle(*this, STYLE_INDENTGUIDE, palette.borderStrong);
    send(message(Scintilla::Message::SetEdgeColour), scintillaColor(palette.borderStrong));
    send(message(Scintilla::Message::MarkerSetFore), bookmarkMarker,
         scintillaColor(palette.accent));
    send(message(Scintilla::Message::MarkerSetBack), bookmarkMarker,
         scintillaColor(palette.accent));
    foldMarkerColor_ = palette.textMuted;
    defineFoldMarkers();
    applyStyle(*this, STYLE_FOLDDISPLAYTEXT, palette.textMuted);
    send(message(Scintilla::Message::StyleSetBack), STYLE_FOLDDISPLAYTEXT,
         scintillaColor(palette.panelSubtle));
    const QString& syntaxComment =
        palette.syntaxComment.isEmpty() ? palette.textMuted : palette.syntaxComment;
    const QString& syntaxString =
        palette.syntaxString.isEmpty() ? palette.positive : palette.syntaxString;
    const QString& syntaxKeyword =
        palette.syntaxKeyword.isEmpty() ? palette.accent : palette.syntaxKeyword;
    const QString& syntaxNumber =
        palette.syntaxNumber.isEmpty() ? palette.warning : palette.syntaxNumber;
    const QString& syntaxType = palette.syntaxType.isEmpty() ? palette.info : palette.syntaxType;
    const QString& syntaxAttribute =
        palette.syntaxAttribute.isEmpty() ? palette.warning : palette.syntaxAttribute;
    embeddedStyleColors_ = {
        scintillaColor(syntaxComment),   scintillaColor(syntaxString),
        scintillaColor(syntaxKeyword),   scintillaColor(palette.danger),
        scintillaColor(syntaxNumber),    scintillaColor(syntaxType),
        scintillaColor(syntaxString),    scintillaColor(syntaxType),
        scintillaColor(syntaxKeyword),   scintillaColor(syntaxType),
        scintillaColor(syntaxAttribute),
    };
    embeddedStyleDirty_ = true;
    send(message(Scintilla::Message::IndicSetFore), findIndicator, scintillaColor(palette.warning));
    send(message(Scintilla::Message::IndicSetFore), selectionIndicator,
         scintillaColor(palette.accent));
    send(message(Scintilla::Message::IndicSetFore), linkIndicator, scintillaColor(syntaxType));

    applyLexerTheme(palette);
    send(message(Scintilla::Message::Colourise), 0, -1);
    updateEmbeddedStyleHighlight();
    themePending_ = false;
}

void EditorWidget::defineFoldMarkers() {
    // Scintilla's Qt layer draws marker images one buffer pixel per logical pixel and ignores
    // the image scale, so the buffer is built at the margin's logical size.
    constexpr int scalePercent = 100;
    const int size = static_cast<int>(symbolMarginWidth);
    send(message(Scintilla::Message::RGBAImageSetWidth), static_cast<uptr_t>(size));
    send(message(Scintilla::Message::RGBAImageSetHeight), static_cast<uptr_t>(size));
    send(message(Scintilla::Message::RGBAImageSetScale), static_cast<uptr_t>(scalePercent));

    // A collapsed block always shows its chevron; open blocks reveal theirs on hover.
    const QByteArray closed = chevronPixels(size, scalePercent, foldMarkerColor_, false);
    for (const auto outline :
         {Scintilla::MarkerOutline::Folder, Scintilla::MarkerOutline::FolderEnd}) {
        sends(message(Scintilla::Message::MarkerDefineRGBAImage), marker(outline),
              closed.constData());
    }
    const QByteArray open = chevronPixels(size, scalePercent, foldMarkerColor_, true);
    for (const auto outline :
         {Scintilla::MarkerOutline::FolderOpen, Scintilla::MarkerOutline::FolderOpenMid}) {
        if (foldMarginHovered_) {
            sends(message(Scintilla::Message::MarkerDefineRGBAImage), marker(outline),
                  open.constData());
        } else {
            send(message(Scintilla::Message::MarkerDefine), marker(outline),
                 static_cast<sptr_t>(Scintilla::MarkerSymbol::Empty));
        }
    }
}

void EditorWidget::setFoldMarginHovered(const bool hovered) {
    if (hovered != foldMarginHovered_) {
        foldMarginHovered_ = hovered;
        defineFoldMarkers();
    }
}

bool EditorWidget::eventFilter(QObject* watched, QEvent* event) {
    if (watched == viewport()) {
        if (event->type() == QEvent::MouseMove) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            sptr_t margins = 0;
            for (uptr_t margin = 0; margin <= foldMargin; ++margin) {
                margins += send(message(Scintilla::Message::GetMarginWidthN), margin);
            }
            const auto position = mouse->position();
            setFoldMarginHovered(position.x() >= 0 && position.x() < margins);
            updateLinkHighlight(position.toPoint(), mouse->modifiers());
        } else if (event->type() == QEvent::MouseButtonPress) {
            auto* mouse = static_cast<QMouseEvent*>(event);
            if (mouse->button() == Qt::LeftButton &&
                mouse->modifiers().testFlag(Qt::ControlModifier)) {
                const auto position = send(message(Scintilla::Message::PositionFromPointClose),
                                           static_cast<uptr_t>(mouse->position().x()),
                                           static_cast<sptr_t>(mouse->position().y()));
                clearLinkHighlight();
                if (position >= 0) {
                    emit definitionRequested(wordAtPosition(position),
                                             fileTokenAtPosition(position),
                                             lineTextAtPosition(position));
                }
                // Scintilla treats Ctrl+click as a selection gesture, so the click stops here.
                return true;
            }
        } else if (event->type() == QEvent::Leave) {
            setFoldMarginHovered(false);
            clearLinkHighlight();
        }
    }
    return ScintillaEditBase::eventFilter(watched, event);
}

void EditorWidget::updateLinkHighlight(const QPoint& point, const Qt::KeyboardModifiers modifiers) {
    if (!modifiers.testFlag(Qt::ControlModifier) || largeFileMode_ || hibernated_) {
        clearLinkHighlight();
        return;
    }
    const auto position = send(message(Scintilla::Message::PositionFromPointClose),
                               static_cast<uptr_t>(point.x()), static_cast<sptr_t>(point.y()));
    if (position < 0 || wordAtPosition(position).isEmpty()) {
        clearLinkHighlight();
        return;
    }
    const auto start =
        send(message(Scintilla::Message::WordStartPosition), static_cast<uptr_t>(position), 1);
    const auto end =
        send(message(Scintilla::Message::WordEndPosition), static_cast<uptr_t>(position), 1);
    if (start == linkStart_ && end == linkEnd_) {
        return;
    }
    clearLinkHighlight();
    linkStart_ = start;
    linkEnd_ = end;
    send(message(Scintilla::Message::SetIndicatorCurrent), linkIndicator);
    send(message(Scintilla::Message::IndicatorFillRange), static_cast<uptr_t>(start), end - start);
    viewport()->setCursor(Qt::PointingHandCursor);
}

void EditorWidget::clearLinkHighlight() {
    if (linkStart_ < 0) {
        return;
    }
    send(message(Scintilla::Message::SetIndicatorCurrent), linkIndicator);
    send(message(Scintilla::Message::IndicatorClearRange), static_cast<uptr_t>(linkStart_),
         linkEnd_ - linkStart_);
    linkStart_ = -1;
    linkEnd_ = -1;
    viewport()->unsetCursor();
}

QString EditorWidget::wordAtPosition(const sptr_t position) const {
    const auto start =
        send(message(Scintilla::Message::WordStartPosition), static_cast<uptr_t>(position), 1);
    const auto end =
        send(message(Scintilla::Message::WordEndPosition), static_cast<uptr_t>(position), 1);
    if (end <= start) {
        return {};
    }
    return QString::fromUtf8(textRange(start, end));
}

QString EditorWidget::fileTokenAtPosition(const sptr_t position) const {
    const auto line =
        send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(position));
    const auto lineStart =
        send(message(Scintilla::Message::PositionFromLine), static_cast<uptr_t>(line));
    const auto lineEnd =
        send(message(Scintilla::Message::GetLineEndPosition), static_cast<uptr_t>(line));
    if (position < lineStart || position > lineEnd) {
        return {};
    }
    const QByteArray text = textRange(lineStart, lineEnd);
    const auto isTokenCharacter = [](const char character) {
        return isWordCharacter(static_cast<unsigned char>(character)) || character == '.' ||
               character == '/' || character == '-' || character == '@' || character == '~' ||
               character == '+';
    };
    if (text.isEmpty()) {
        return {};
    }
    const qsizetype offset = static_cast<qsizetype>(position - lineStart);
    const qsizetype index = qMax<qsizetype>(0, qMin(offset, text.size() - 1));
    if (!isTokenCharacter(text.at(index))) {
        return {};
    }
    qsizetype start = index;
    while (start > 0 && isTokenCharacter(text.at(start - 1))) {
        --start;
    }
    qsizetype end = index;
    while (end + 1 < text.size() && isTokenCharacter(text.at(end + 1))) {
        ++end;
    }
    return QString::fromUtf8(text.mid(start, end - start + 1));
}

QString EditorWidget::lineTextAtPosition(const sptr_t position) const {
    const auto line =
        send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(position));
    const auto start =
        send(message(Scintilla::Message::PositionFromLine), static_cast<uptr_t>(line));
    const auto end =
        send(message(Scintilla::Message::GetLineEndPosition), static_cast<uptr_t>(line));
    return end > start ? QString::fromUtf8(textRange(start, end)) : QString();
}

sptr_t EditorWidget::caretWordPosition() const {
    return send(message(Scintilla::Message::GetCurrentPos));
}

void EditorWidget::updateEmbeddedStyleHighlight() {
    // Lexilla's HTML lexer leaves <style> bodies unstyled, so the visible CSS is colored with
    // a value-colored text indicator. Only the lines on screen are scanned.
    const bool active =
        lexerName_ == QStringLiteral("hypertext") && !largeFileMode_ && !hibernated_;
    send(message(Scintilla::Message::SetIndicatorCurrent), embeddedStyleIndicator);
    const auto length = send(message(Scintilla::Message::GetLength));
    if (!active) {
        if (embeddedStyleActive_) {
            send(message(Scintilla::Message::IndicatorClearRange), 0, length);
            embeddedStyleActive_ = false;
        }
        return;
    }
    embeddedStyleActive_ = true;

    const auto firstDisplayLine = send(message(Scintilla::Message::GetFirstVisibleLine));
    const auto linesOnScreen = send(message(Scintilla::Message::LinesOnScreen));
    const auto firstLine = send(message(Scintilla::Message::DocLineFromVisible),
                                static_cast<uptr_t>(firstDisplayLine));
    const auto lastLine = send(message(Scintilla::Message::DocLineFromVisible),
                               static_cast<uptr_t>(firstDisplayLine + linesOnScreen + 1));
    const sptr_t visibleStart =
        send(message(Scintilla::Message::PositionFromLine), static_cast<uptr_t>(firstLine));
    const sptr_t visibleEnd = qMin(length, send(message(Scintilla::Message::GetLineEndPosition),
                                                static_cast<uptr_t>(lastLine)));
    if (visibleStart < 0 || visibleEnd <= visibleStart) {
        return;
    }
    // Filling indicators repaints and reports another UI update, so unchanged ranges stop here.
    if (!embeddedStyleDirty_ && visibleStart == embeddedStyleStart_ &&
        visibleEnd == embeddedStyleEnd_) {
        return;
    }
    embeddedStyleDirty_ = false;
    embeddedStyleStart_ = visibleStart;
    embeddedStyleEnd_ = visibleEnd;
    send(message(Scintilla::Message::IndicatorClearRange), static_cast<uptr_t>(visibleStart),
         visibleEnd - visibleStart);

    const sptr_t base = qMax<sptr_t>(0, visibleStart - embeddedStyleLookBehind);
    const QByteArray text = textRange(base, visibleEnd);
    const QByteArray lower = text.toLower();
    const qsizetype size = text.size();

    enum Token {
        Comment,
        String,
        AtRule,
        Important,
        Number,
        Function,
        Value,
        Property,
        Selector,
        ClassName,
        Pseudo
    };
    const auto fill = [&](const qsizetype from, const qsizetype to, const Token token) {
        const sptr_t start = qMax(base + from, visibleStart);
        const sptr_t end = qMin(base + to, visibleEnd);
        if (end > start) {
            send(message(Scintilla::Message::SetIndicatorValue),
                 static_cast<uptr_t>(embeddedStyleColors_[token] | SC_INDICVALUEBIT));
            send(message(Scintilla::Message::IndicatorFillRange), static_cast<uptr_t>(start),
                 end - start);
        }
    };
    const auto nameEnd = [&](qsizetype index) {
        while (index < size && isCssNameCharacter(text.at(index))) {
            ++index;
        }
        return index;
    };

    const auto colorBlock = [&](const qsizetype blockStart, const qsizetype blockEnd) {
        int depth = 0;
        bool inValue = false;
        qsizetype index = blockStart;
        while (index < blockEnd) {
            const char character = text.at(index);
            const char next = index + 1 < blockEnd ? text.at(index + 1) : '\0';
            if (character == '/' && next == '*') {
                const qsizetype close = text.indexOf("*/", index + 2);
                const qsizetype end = close < 0 || close >= blockEnd ? blockEnd : close + 2;
                fill(index, end, Comment);
                index = end;
            } else if (character == '"' || character == '\'') {
                qsizetype end = index + 1;
                while (end < blockEnd && text.at(end) != character && text.at(end) != '\n') {
                    end += text.at(end) == '\\' ? 2 : 1;
                }
                end = qMin(end + 1, blockEnd);
                fill(index, end, String);
                index = end;
            } else if (character == '{') {
                ++depth;
                inValue = false;
                ++index;
            } else if (character == '}') {
                depth = qMax(0, depth - 1);
                inValue = false;
                ++index;
            } else if (character == ';') {
                inValue = false;
                ++index;
            } else if (character == '@') {
                const qsizetype end = nameEnd(index + 1);
                fill(index, end, AtRule);
                index = end;
            } else if (character == '!') {
                const qsizetype end = nameEnd(index + 1);
                fill(index, end, Important);
                index = qMax(end, index + 1);
            } else if (inValue) {
                if (character == '#' || isDigit(character) ||
                    ((character == '.' || character == '-') && isDigit(next))) {
                    qsizetype end = index + 1;
                    while (end < blockEnd && (isCssNameCharacter(text.at(end)) ||
                                              text.at(end) == '.' || text.at(end) == '%')) {
                        ++end;
                    }
                    fill(index, end, Number);
                    index = end;
                } else if (isCssNameCharacter(character)) {
                    const qsizetype end = nameEnd(index);
                    fill(index, end, end < blockEnd && text.at(end) == '(' ? Function : Value);
                    index = end;
                } else {
                    ++index;
                }
            } else if (isCssNameCharacter(character)) {
                const qsizetype end = nameEnd(index);
                bool property = false;
                if (depth > 0) {
                    // Inside a rule a name followed by ':' is a property, unless a '{' shows
                    // it is a nested selector such as `a:hover {`.
                    qsizetype scan = end;
                    while (scan < blockEnd && text.at(scan) != ';' && text.at(scan) != '{' &&
                           text.at(scan) != '}') {
                        ++scan;
                    }
                    qsizetype colon = end;
                    while (colon < blockEnd && (text.at(colon) == ' ' || text.at(colon) == '\t')) {
                        ++colon;
                    }
                    property = colon < blockEnd && text.at(colon) == ':' &&
                               (scan >= blockEnd || text.at(scan) != '{');
                    if (property) {
                        fill(index, end, Property);
                        inValue = true;
                        index = colon + 1;
                        continue;
                    }
                }
                fill(index, end, Selector);
                index = end;
            } else if ((character == '.' || character == '#') && isCssNameCharacter(next)) {
                const qsizetype end = nameEnd(index + 1);
                fill(index, end, ClassName);
                index = end;
            } else if (character == ':') {
                qsizetype start = index + 1;
                if (start < blockEnd && text.at(start) == ':') {
                    ++start;
                }
                const qsizetype end = nameEnd(start);
                fill(index, end, Pseudo);
                index = qMax(end, index + 1);
            } else {
                ++index;
            }
        }
    };

    qsizetype searchFrom = 0;
    while (searchFrom < size) {
        const qsizetype open = lower.indexOf("<style", searchFrom);
        if (open < 0) {
            break;
        }
        const char after = open + 6 < size ? lower.at(open + 6) : '\0';
        const qsizetype tagEnd = lower.indexOf('>', open);
        if (tagEnd < 0) {
            break;
        }
        if (after != '>' && after != ' ' && after != '\t' && after != '\n' && after != '\r') {
            searchFrom = open + 6;
            continue;
        }
        // Skip "<style" text inside scripts, strings or comments.
        const sptr_t tagPosition = base + open + 1;
        send(message(Scintilla::Message::Colourise), static_cast<uptr_t>(tagPosition),
             tagPosition + 1);
        const auto tagStyle =
            send(message(Scintilla::Message::GetStyleAt), static_cast<uptr_t>(tagPosition));
        if (tagStyle != SCE_H_TAG && tagStyle != SCE_H_TAGUNKNOWN) {
            searchFrom = open + 6;
            continue;
        }
        const qsizetype close = lower.indexOf("</style", tagEnd + 1);
        const qsizetype blockEnd = close < 0 ? size : close;
        if (base + blockEnd > visibleStart) {
            colorBlock(tagEnd + 1, blockEnd);
        }
        if (close < 0) {
            break;
        }
        searchFrom = close + 7;
    }
}

void EditorWidget::undoEdit() { send(message(Scintilla::Message::Undo)); }

void EditorWidget::redoEdit() { send(message(Scintilla::Message::Redo)); }

void EditorWidget::cutSelection() { send(message(Scintilla::Message::Cut)); }

void EditorWidget::copySelection() { send(message(Scintilla::Message::Copy)); }

void EditorWidget::pasteClipboard() { send(message(Scintilla::Message::Paste)); }

void EditorWidget::deleteSelection() { send(message(Scintilla::Message::Clear)); }

void EditorWidget::selectAllText() { send(message(Scintilla::Message::SelectAll)); }

void EditorWidget::duplicateLines() {
    const auto selectionStart = send(message(Scintilla::Message::GetSelectionStart));
    const auto selectionEnd = send(message(Scintilla::Message::GetSelectionEnd));
    if (selectionStart != selectionEnd &&
        send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(selectionStart)) ==
            send(message(Scintilla::Message::LineFromPosition),
                 static_cast<uptr_t>(selectionEnd))) {
        send(message(Scintilla::Message::SelectionDuplicate));
        return;
    }

    const auto range = selectedLineRange(false);
    const auto blockStart =
        send(message(Scintilla::Message::PositionFromLine), static_cast<uptr_t>(range.first));
    const auto blockEnd =
        send(message(Scintilla::Message::GetLineEndPosition), static_cast<uptr_t>(range.last));
    const QByteArray block = endOfLine() + textRange(blockStart, blockEnd);
    sends(message(Scintilla::Message::InsertText), static_cast<uptr_t>(blockEnd),
          block.constData());
}

void EditorWidget::moveLinesUp() { send(message(Scintilla::Message::MoveSelectedLinesUp)); }

void EditorWidget::moveLinesDown() { send(message(Scintilla::Message::MoveSelectedLinesDown)); }

void EditorWidget::deleteLines() {
    const auto range = selectedLineRange(false);
    const auto totalLines = send(message(Scintilla::Message::GetLineCount));
    auto start =
        send(message(Scintilla::Message::PositionFromLine), static_cast<uptr_t>(range.first));
    const bool includesLastLine = range.last + 1 >= totalLines;
    const auto end = includesLastLine ? send(message(Scintilla::Message::GetLength))
                                      : send(message(Scintilla::Message::PositionFromLine),
                                             static_cast<uptr_t>(range.last + 1));
    if (includesLastLine && range.first > 0) {
        // Remove the preceding line break so no empty trailing line is left.
        start = send(message(Scintilla::Message::GetLineEndPosition),
                     static_cast<uptr_t>(range.first - 1));
    }
    send(message(Scintilla::Message::DeleteRange), static_cast<uptr_t>(start), end - start);
}

void EditorWidget::joinLines() {
    auto range = selectedLineRange(false);
    if (range.first == range.last) {
        range.last =
            qMin<sptr_t>(range.first + 1, send(message(Scintilla::Message::GetLineCount)) - 1);
    }
    if (range.first == range.last) {
        return;
    }

    const auto lines = linesInRange(range);
    QByteArray joined = lines.first();
    trimTrailingSpaces(joined);
    for (qsizetype index = 1; index < lines.size(); ++index) {
        const QByteArray part = lines.at(index).trimmed();
        if (part.isEmpty()) {
            continue;
        }
        if (!joined.trimmed().isEmpty()) {
            joined.append(' ');
        }
        joined.append(part);
    }
    replaceLines(range, {joined});
}

void EditorWidget::sortLines(const bool removeDuplicates) {
    auto range = selectedLineRange(true);
    if (range.last > range.first && linesInRange({range.last, range.last}).first().isEmpty()) {
        // Keep the final line break in place instead of sorting it to the top.
        --range.last;
    }
    if (range.first >= range.last) {
        return;
    }

    auto lines = linesInRange(range);
    std::stable_sort(lines.begin(), lines.end());
    if (removeDuplicates) {
        lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    }
    replaceLines(range, lines);
}

void EditorWidget::trimTrailingWhitespace() {
    const auto range = selectedLineRange(true);
    send(message(Scintilla::Message::BeginUndoAction));
    for (sptr_t line = range.last; line >= range.first; --line) {
        const auto lineStart =
            send(message(Scintilla::Message::PositionFromLine), static_cast<uptr_t>(line));
        const auto lineEnd =
            send(message(Scintilla::Message::GetLineEndPosition), static_cast<uptr_t>(line));
        auto trimmedEnd = lineEnd;
        while (trimmedEnd > lineStart &&
               isHorizontalSpace(static_cast<char>(charAt(trimmedEnd - 1)))) {
            --trimmedEnd;
        }
        if (trimmedEnd < lineEnd) {
            send(message(Scintilla::Message::DeleteRange), static_cast<uptr_t>(trimmedEnd),
                 lineEnd - trimmedEnd);
        }
    }
    send(message(Scintilla::Message::EndUndoAction));
}

void EditorWidget::convertCase(const bool upper) {
    if (!hasSelection()) {
        const auto caret = send(message(Scintilla::Message::GetCurrentPos));
        const auto wordStart =
            send(message(Scintilla::Message::WordStartPosition), static_cast<uptr_t>(caret), 1);
        const auto wordEnd =
            send(message(Scintilla::Message::WordEndPosition), static_cast<uptr_t>(caret), 1);
        if (wordStart == wordEnd) {
            return;
        }
        send(message(Scintilla::Message::SetSel), static_cast<uptr_t>(wordStart), wordEnd);
    }
    send(message(upper ? Scintilla::Message::UpperCase : Scintilla::Message::LowerCase));
}

bool EditorWidget::toggleComment() {
    const auto tokens = commentTokensForSyntax(syntaxName_);
    if (tokens.open.isEmpty()) {
        return false;
    }

    const auto range = selectedLineRange(false);
    auto lines = linesInRange(range);
    bool hasContent = false;
    bool allCommented = true;
    qsizetype indentSize = std::numeric_limits<qsizetype>::max();
    for (const auto& line : lines) {
        const QByteArray body = line.trimmed();
        if (body.isEmpty()) {
            continue;
        }
        hasContent = true;
        allCommented = allCommented && body.startsWith(tokens.open) &&
                       (tokens.close.isEmpty() || body.endsWith(tokens.close));
        qsizetype indent = 0;
        while (indent < line.size() && isHorizontalSpace(line.at(indent))) {
            ++indent;
        }
        indentSize = qMin(indentSize, indent);
    }
    if (!hasContent) {
        return false;
    }

    for (auto& line : lines) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        if (allCommented) {
            const qsizetype openAt = line.indexOf(tokens.open);
            qsizetype removeLength = tokens.open.size();
            if (openAt + removeLength < line.size() && line.at(openAt + removeLength) == ' ') {
                ++removeLength;
            }
            line.remove(openAt, removeLength);
            if (!tokens.close.isEmpty()) {
                trimTrailingSpaces(line);
                line.chop(tokens.close.size());
                trimTrailingSpaces(line);
            }
        } else {
            line.insert(indentSize, tokens.open + ' ');
            if (!tokens.close.isEmpty()) {
                line.append(' ' + tokens.close);
            }
        }
    }
    replaceLines(range, lines);
    return true;
}

int EditorWidget::selectionCount() const {
    return static_cast<int>(send(message(Scintilla::Message::GetSelections)));
}

void EditorWidget::addNextOccurrence() {
    if (!hasSelection() && selectionCount() == 1) {
        // The first press selects the word under the caret, like Sublime Text.
        const auto caret = send(message(Scintilla::Message::GetCurrentPos));
        const auto wordStart =
            send(message(Scintilla::Message::WordStartPosition), static_cast<uptr_t>(caret), 1);
        const auto wordEnd =
            send(message(Scintilla::Message::WordEndPosition), static_cast<uptr_t>(caret), 1);
        if (wordStart != wordEnd) {
            send(message(Scintilla::Message::SetSel), static_cast<uptr_t>(wordStart), wordEnd);
        }
        return;
    }
    send(message(Scintilla::Message::TargetWholeDocument));
    setSearchOptions(SearchOptions{.matchCase = true});
    send(message(Scintilla::Message::MultipleSelectAddNext));
    send(message(Scintilla::Message::ScrollCaret));
}

void EditorWidget::selectAllOccurrences() {
    if (!hasSelection() && selectionCount() == 1) {
        addNextOccurrence();
        if (!hasSelection()) {
            return;
        }
    }
    send(message(Scintilla::Message::TargetWholeDocument));
    setSearchOptions(SearchOptions{.matchCase = true});
    send(message(Scintilla::Message::MultipleSelectAddEach));
}

void EditorWidget::addCursorVertically(const bool above) {
    // Grow from the selection farthest in the requested direction.
    sptr_t edgeCaret = send(message(Scintilla::Message::GetSelectionNCaret), 0);
    sptr_t edgeLine =
        send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(edgeCaret));
    const int count = selectionCount();
    for (int index = 1; index < count; ++index) {
        const auto caret =
            send(message(Scintilla::Message::GetSelectionNCaret), static_cast<uptr_t>(index));
        const auto line =
            send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(caret));
        if (above ? line < edgeLine : line > edgeLine) {
            edgeCaret = caret;
            edgeLine = line;
        }
    }

    const sptr_t targetLine = edgeLine + (above ? -1 : 1);
    if (targetLine < 0 || targetLine >= send(message(Scintilla::Message::GetLineCount))) {
        return;
    }
    const auto column =
        send(message(Scintilla::Message::GetColumn), static_cast<uptr_t>(edgeCaret));
    const auto position =
        send(message(Scintilla::Message::FindColumn), static_cast<uptr_t>(targetLine), column);
    send(message(Scintilla::Message::AddSelection), static_cast<uptr_t>(position), position);
    send(message(Scintilla::Message::ScrollCaret));
}

void EditorWidget::splitSelectionIntoLines() {
    const auto selectionStart = send(message(Scintilla::Message::GetSelectionStart));
    const auto selectionEnd = send(message(Scintilla::Message::GetSelectionEnd));
    const auto range = selectedLineRange(false);
    if (selectionStart == selectionEnd || range.first == range.last) {
        return;
    }

    for (sptr_t line = range.first; line <= range.last; ++line) {
        const auto start = qMax(selectionStart, send(message(Scintilla::Message::PositionFromLine),
                                                     static_cast<uptr_t>(line)));
        const auto end = qMin(selectionEnd, send(message(Scintilla::Message::GetLineEndPosition),
                                                 static_cast<uptr_t>(line)));
        send(message(line == range.first ? Scintilla::Message::SetSelection
                                         : Scintilla::Message::AddSelection),
             static_cast<uptr_t>(end), start);
    }
}

void EditorWidget::collapseToMainSelection() {
    send(message(Scintilla::Message::SetEmptySelection),
         static_cast<uptr_t>(send(message(Scintilla::Message::GetCurrentPos))));
}

bool EditorWidget::showWordCompletions(const bool explicitRequest) {
    if (largeFileMode_ || hibernated_ || selectionCount() > 1) {
        return false;
    }

    const auto caret = send(message(Scintilla::Message::GetCurrentPos));
    const auto wordStart =
        send(message(Scintilla::Message::WordStartPosition), static_cast<uptr_t>(caret), 1);
    const auto prefixLength = caret - wordStart;
    if (prefixLength < (explicitRequest ? 1 : minimumCompletionPrefix)) {
        return false;
    }
    const QByteArray prefix = textRange(wordStart, caret).toLower();

    // Scan a bounded window around the caret so completion stays cheap on long files.
    constexpr sptr_t scanRadius = 64 * 1024;
    const auto scanStart = qMax<sptr_t>(0, caret - scanRadius);
    const auto scanEnd = qMin(send(message(Scintilla::Message::GetLength)), caret + scanRadius);
    const QByteArray text = textRange(scanStart, scanEnd);
    const auto isWordByte = [&text](const qsizetype index) {
        return isWordCharacter(static_cast<unsigned char>(text.at(index)));
    };

    QList<QByteArray> words;
    QSet<QByteArray> seen;
    qsizetype index = 0;
    while (index < text.size()) {
        if (!isWordByte(index)) {
            ++index;
            continue;
        }
        qsizetype end = index;
        while (end < text.size() && isWordByte(end)) {
            ++end;
        }
        const bool isCurrentWord = scanStart + index == wordStart;
        const bool startsWithDigit = text.at(index) >= '0' && text.at(index) <= '9';
        if (!isCurrentWord && !startsWithDigit && end - index > prefix.size()) {
            const QByteArray word = text.mid(index, end - index);
            if (word.toLower().startsWith(prefix) && !seen.contains(word)) {
                seen.insert(word);
                words.append(word);
            }
        }
        index = end;
    }

    if (words.isEmpty()) {
        if (send(message(Scintilla::Message::AutoCActive)) != 0) {
            send(message(Scintilla::Message::AutoCCancel));
        }
        return false;
    }
    std::sort(words.begin(), words.end(), [](const QByteArray& left, const QByteArray& right) {
        return left.compare(right, Qt::CaseInsensitive) < 0;
    });
    if (words.size() > maximumCompletionItems) {
        words.resize(maximumCompletionItems);
    }
    const QByteArray list = words.join(' ');
    sends(message(Scintilla::Message::AutoCShow), static_cast<uptr_t>(prefixLength),
          list.constData());
    return true;
}

int EditorWidget::lineCount() const {
    return static_cast<int>(send(message(Scintilla::Message::GetLineCount)));
}

int EditorWidget::currentLine() const {
    const auto caret = send(message(Scintilla::Message::GetCurrentPos));
    return static_cast<int>(
               send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(caret))) +
           1;
}

void EditorWidget::goToLine(const int line) {
    const sptr_t lastLine = send(message(Scintilla::Message::GetLineCount)) - 1;
    const sptr_t target = std::clamp<sptr_t>(line - 1, 0, lastLine);
    send(message(Scintilla::Message::EnsureVisibleEnforcePolicy), static_cast<uptr_t>(target));
    send(message(Scintilla::Message::GotoLine), static_cast<uptr_t>(target));
}

sptr_t EditorWidget::caretPosition() const {
    return send(message(Scintilla::Message::GetCurrentPos));
}

void EditorWidget::setCaretPosition(const sptr_t position) {
    const sptr_t target =
        std::clamp<sptr_t>(position, 0, send(message(Scintilla::Message::GetLength)));
    send(message(Scintilla::Message::EnsureVisibleEnforcePolicy),
         static_cast<uptr_t>(
             send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(target))));
    send(message(Scintilla::Message::GotoPos), static_cast<uptr_t>(target));
}

bool EditorWidget::jumpToMatchingBrace() {
    const auto caret = send(message(Scintilla::Message::GetCurrentPos));
    sptr_t brace = -1;
    if (isBrace(charAt(caret))) {
        brace = caret;
    } else if (caret > 0 && isBrace(charAt(caret - 1))) {
        brace = caret - 1;
    }
    if (brace < 0) {
        return false;
    }

    const auto match = send(message(Scintilla::Message::BraceMatch), static_cast<uptr_t>(brace), 0);
    if (match < 0) {
        return false;
    }
    send(message(Scintilla::Message::SetEmptySelection), static_cast<uptr_t>(match));
    send(message(Scintilla::Message::ScrollCaret));
    return true;
}

void EditorWidget::toggleBookmark() {
    const auto caret = send(message(Scintilla::Message::GetCurrentPos));
    toggleBookmarkAtLine(
        send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(caret)));
}

bool EditorWidget::hasBookmark(const int line) const {
    return (send(message(Scintilla::Message::MarkerGet), static_cast<uptr_t>(line - 1)) &
            bookmarkMask) != 0;
}

bool EditorWidget::goToNextBookmark() {
    const auto line = static_cast<sptr_t>(currentLine() - 1);
    auto found =
        send(message(Scintilla::Message::MarkerNext), static_cast<uptr_t>(line + 1), bookmarkMask);
    if (found < 0) {
        found = send(message(Scintilla::Message::MarkerNext), 0, bookmarkMask);
    }
    if (found < 0) {
        return false;
    }
    goToLine(static_cast<int>(found + 1));
    return true;
}

bool EditorWidget::goToPreviousBookmark() {
    const auto line = static_cast<sptr_t>(currentLine() - 1);
    auto found = line > 0 ? send(message(Scintilla::Message::MarkerPrevious),
                                 static_cast<uptr_t>(line - 1), bookmarkMask)
                          : -1;
    if (found < 0) {
        found = send(message(Scintilla::Message::MarkerPrevious),
                     static_cast<uptr_t>(send(message(Scintilla::Message::GetLineCount)) - 1),
                     bookmarkMask);
    }
    if (found < 0) {
        return false;
    }
    goToLine(static_cast<int>(found + 1));
    return true;
}

void EditorWidget::clearBookmarks() {
    send(message(Scintilla::Message::MarkerDeleteAll), bookmarkMarker);
}

void EditorWidget::setAllFoldsExpanded(const bool expanded) {
    send(message(Scintilla::Message::FoldAll),
         static_cast<uptr_t>(expanded ? Scintilla::FoldAction::Expand
                                      : Scintilla::FoldAction::Contract));
}

FindResult EditorWidget::findText(const QString& query, const bool backwards, const bool matchCase,
                                  const bool wholeWord) {
    return findText(query, backwards,
                    SearchOptions{.matchCase = matchCase, .wholeWord = wholeWord});
}

FindResult EditorWidget::findText(const QString& query, const bool backwards,
                                  const SearchOptions& options) {
    const QByteArray needle = query.toUtf8();
    if (needle.isEmpty()) {
        return {};
    }

    setSearchOptions(options);
    const auto [lowerBound, upperBound] = searchBounds(options);
    const auto selectionStart =
        qBound(lowerBound, send(message(Scintilla::Message::GetSelectionStart)), upperBound);
    const auto selectionEnd =
        qBound(lowerBound, send(message(Scintilla::Message::GetSelectionEnd)), upperBound);

    const auto searchRange = [this, &needle](const sptr_t start, const sptr_t end) {
        send(message(Scintilla::Message::SetTargetStart), static_cast<uptr_t>(start));
        send(message(Scintilla::Message::SetTargetEnd), static_cast<uptr_t>(end));
        return sends(message(Scintilla::Message::SearchInTarget),
                     static_cast<uptr_t>(needle.size()), needle.constData());
    };

    sptr_t match =
        backwards ? searchRange(selectionStart, lowerBound) : searchRange(selectionEnd, upperBound);
    if (!backwards && match >= 0 && selectionStart == selectionEnd && match == selectionEnd &&
        send(message(Scintilla::Message::GetTargetEnd)) == selectionEnd &&
        selectionEnd < upperBound) {
        // Step past an empty regex match so repeated searches make progress.
        match = searchRange(
            send(message(Scintilla::Message::PositionAfter), static_cast<uptr_t>(selectionEnd)),
            upperBound);
    }
    bool wrapped = false;
    if (match < 0) {
        match = backwards ? searchRange(upperBound, selectionEnd)
                          : searchRange(lowerBound, selectionStart);
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
    return replaceSelection(query, replacement,
                            SearchOptions{.matchCase = matchCase, .wholeWord = wholeWord});
}

bool EditorWidget::replaceSelection(const QString& query, const QString& replacement,
                                    const SearchOptions& options) {
    const QByteArray needle = query.toUtf8();
    if (needle.isEmpty() || !selectionMatches(needle, options)) {
        return false;
    }

    const auto matchLength = send(message(Scintilla::Message::GetTargetEnd)) -
                             send(message(Scintilla::Message::GetTargetStart));
    const auto insertedLength = replaceTarget(replacement.toUtf8(), options.regex);
    adjustSearchScope(insertedLength - matchLength);
    send(message(Scintilla::Message::SetEmptySelection),
         static_cast<uptr_t>(send(message(Scintilla::Message::GetTargetEnd))));
    return true;
}

int EditorWidget::replaceAll(const QString& query, const QString& replacement, const bool matchCase,
                             const bool wholeWord) {
    return replaceAll(query, replacement,
                      SearchOptions{.matchCase = matchCase, .wholeWord = wholeWord});
}

int EditorWidget::replaceAll(const QString& query, const QString& replacement,
                             const SearchOptions& options) {
    const QByteArray needle = query.toUtf8();
    if (needle.isEmpty()) {
        return 0;
    }
    const QByteArray replacementBytes = replacement.toUtf8();
    setSearchOptions(options);

    int replacements = 0;
    auto [searchStart, searchEnd] = searchBounds(options);
    send(message(Scintilla::Message::BeginUndoAction));
    while (searchStart <= searchEnd) {
        send(message(Scintilla::Message::SetTargetStart), static_cast<uptr_t>(searchStart));
        send(message(Scintilla::Message::SetTargetEnd), static_cast<uptr_t>(searchEnd));
        const auto match = sends(message(Scintilla::Message::SearchInTarget),
                                 static_cast<uptr_t>(needle.size()), needle.constData());
        if (match < 0) {
            break;
        }

        const auto matchStart = send(message(Scintilla::Message::GetTargetStart));
        const auto matchLength = send(message(Scintilla::Message::GetTargetEnd)) - matchStart;
        const auto insertedLength = replaceTarget(replacementBytes, options.regex);
        ++replacements;
        searchEnd += insertedLength - matchLength;
        adjustSearchScope(insertedLength - matchLength);
        searchStart = matchStart + insertedLength;
        if (matchLength == 0) {
            if (searchStart >= searchEnd) {
                break;
            }
            searchStart =
                send(message(Scintilla::Message::PositionAfter), static_cast<uptr_t>(searchStart));
        }
    }
    send(message(Scintilla::Message::EndUndoAction));
    return replacements;
}

void EditorWidget::setSearchScopeToSelection() {
    const auto selectionStart = send(message(Scintilla::Message::GetSelectionStart));
    const auto selectionEnd = send(message(Scintilla::Message::GetSelectionEnd));
    if (selectionStart == selectionEnd) {
        clearSearchScope();
        return;
    }
    searchScopeStart_ = selectionStart;
    searchScopeEnd_ = selectionEnd;
    searchScopeActive_ = true;
}

void EditorWidget::clearSearchScope() noexcept {
    searchScopeStart_ = 0;
    searchScopeEnd_ = 0;
    searchScopeActive_ = false;
}

bool EditorWidget::hasSearchScope() const noexcept { return searchScopeActive_; }

int EditorWidget::highlightMatches(const QString& query, const SearchOptions& options) {
    clearMatchHighlights();
    const QByteArray needle = query.toUtf8();
    if (needle.isEmpty() || hibernated_) {
        return 0;
    }

    setSearchOptions(options);
    auto [searchStart, searchEnd] = searchBounds(options);
    if (largeFileMode_) {
        // Only mark the visible lines so typing a query never scans a huge buffer.
        const auto firstLine =
            send(message(Scintilla::Message::DocLineFromVisible),
                 static_cast<uptr_t>(send(message(Scintilla::Message::GetFirstVisibleLine))));
        const auto lastLine = qMin(firstLine + send(message(Scintilla::Message::LinesOnScreen)) + 1,
                                   send(message(Scintilla::Message::GetLineCount)) - 1);
        searchStart = qMax(searchStart, send(message(Scintilla::Message::PositionFromLine),
                                             static_cast<uptr_t>(firstLine)));
        searchEnd = qMin(searchEnd, send(message(Scintilla::Message::GetLineEndPosition),
                                         static_cast<uptr_t>(lastLine)));
    }

    send(message(Scintilla::Message::SetIndicatorCurrent), findIndicator);
    int count = 0;
    while (searchStart < searchEnd && count < highlightMatchLimit) {
        send(message(Scintilla::Message::SetTargetStart), static_cast<uptr_t>(searchStart));
        send(message(Scintilla::Message::SetTargetEnd), static_cast<uptr_t>(searchEnd));
        if (sends(message(Scintilla::Message::SearchInTarget), static_cast<uptr_t>(needle.size()),
                  needle.constData()) < 0) {
            break;
        }
        const auto matchStart = send(message(Scintilla::Message::GetTargetStart));
        const auto matchEnd = send(message(Scintilla::Message::GetTargetEnd));
        if (matchEnd > matchStart) {
            send(message(Scintilla::Message::IndicatorFillRange), static_cast<uptr_t>(matchStart),
                 matchEnd - matchStart);
            ++count;
            searchStart = matchEnd;
        } else {
            searchStart =
                send(message(Scintilla::Message::PositionAfter), static_cast<uptr_t>(matchStart));
        }
    }
    return count;
}

void EditorWidget::clearMatchHighlights() {
    send(message(Scintilla::Message::SetIndicatorCurrent), findIndicator);
    send(message(Scintilla::Message::IndicatorClearRange), 0,
         send(message(Scintilla::Message::GetLength)));
}

void EditorWidget::configureEditor() {
    send(message(Scintilla::Message::SetCodePage), 65001);
    send(message(Scintilla::Message::SetMargins), 3);
    send(message(Scintilla::Message::SetMarginTypeN), 0,
         static_cast<uptr_t>(Scintilla::MarginType::Number));
    send(message(Scintilla::Message::SetMarginMaskN), 0, 0);
    send(message(Scintilla::Message::SetMarginSensitiveN), 0, 0);
    updateLineNumberMarginWidth();

    send(message(Scintilla::Message::SetCaretWidth), 2);
    send(message(Scintilla::Message::SetScrollWidth), 1);
    send(message(Scintilla::Message::SetScrollWidthTracking), 1);
    send(message(Scintilla::Message::SetEndAtLastLine), 0);
    send(message(Scintilla::Message::SetTabIndents), 1);
    send(message(Scintilla::Message::SetBackSpaceUnIndents), 1);

    // Ctrl/Cmd+click adds carets, Alt+drag makes a column selection, and typing or pasting
    // applies to every selection.
    send(message(Scintilla::Message::SetMultipleSelection), 1);
    send(message(Scintilla::Message::SetAdditionalSelectionTyping), 1);
    send(message(Scintilla::Message::SetMultiPaste),
         static_cast<uptr_t>(Scintilla::MultiPaste::Each));
    send(message(Scintilla::Message::SetMouseSelectionRectangularSwitch), 1);
    send(message(Scintilla::Message::SetVirtualSpaceOptions),
         static_cast<uptr_t>(Scintilla::VirtualSpace::RectangularSelection));

    send(message(Scintilla::Message::AutoCSetIgnoreCase), 1);
    send(message(Scintilla::Message::AutoCSetAutoHide), 1);
    send(message(Scintilla::Message::AutoCSetMaxHeight), 8);

    send(message(Scintilla::Message::SetMarginTypeN), bookmarkMargin,
         static_cast<sptr_t>(Scintilla::MarginType::Symbol));
    send(message(Scintilla::Message::SetMarginMaskN), bookmarkMargin, bookmarkMask);
    send(message(Scintilla::Message::SetMarginWidthN), bookmarkMargin, symbolMarginWidth);
    send(message(Scintilla::Message::SetMarginSensitiveN), bookmarkMargin, 1);
    send(message(Scintilla::Message::MarkerDefine), bookmarkMarker,
         static_cast<sptr_t>(Scintilla::MarkerSymbol::Bookmark));

    send(message(Scintilla::Message::SetMarginTypeN), foldMargin,
         static_cast<sptr_t>(Scintilla::MarginType::Symbol));
    send(message(Scintilla::Message::SetMarginMaskN), foldMargin, Scintilla::MaskFolders);
    send(message(Scintilla::Message::SetMarginWidthN), foldMargin, 0);
    send(message(Scintilla::Message::SetMarginSensitiveN), foldMargin, 1);
    // The body of a block stays unmarked so the margin reads as a gutter, not a tree outline.
    for (const auto outline :
         {Scintilla::MarkerOutline::FolderMidTail, Scintilla::MarkerOutline::FolderSub,
          Scintilla::MarkerOutline::FolderTail}) {
        send(message(Scintilla::Message::MarkerDefine), marker(outline),
             static_cast<sptr_t>(Scintilla::MarkerSymbol::Empty));
    }
    defineFoldMarkers();
    send(message(Scintilla::Message::SetAutomaticFold),
         static_cast<uptr_t>(Scintilla::AutomaticFold::Show) |
             static_cast<uptr_t>(Scintilla::AutomaticFold::Click) |
             static_cast<uptr_t>(Scintilla::AutomaticFold::Change));
    // A collapsed block shows a small boxed ellipsis instead of a full-width rule.
    send(message(Scintilla::Message::SetFoldFlags), 0);
    sends(message(Scintilla::Message::SetDefaultFoldDisplayText), 0, "\xE2\x80\xA6");
    send(message(Scintilla::Message::FoldDisplayTextSetStyle),
         static_cast<uptr_t>(Scintilla::FoldDisplayTextStyle::Boxed));

    send(message(Scintilla::Message::IndicSetStyle), linkIndicator,
         static_cast<sptr_t>(Scintilla::IndicatorStyle::Plain));
    send(message(Scintilla::Message::IndicSetStyle), embeddedStyleIndicator,
         static_cast<sptr_t>(Scintilla::IndicatorStyle::TextFore));
    send(message(Scintilla::Message::IndicSetFlags), embeddedStyleIndicator,
         static_cast<sptr_t>(Scintilla::IndicFlag::ValueFore));

    send(message(Scintilla::Message::IndicSetStyle), findIndicator,
         static_cast<sptr_t>(Scintilla::IndicatorStyle::RoundBox));
    send(message(Scintilla::Message::IndicSetAlpha), findIndicator, 70);
    send(message(Scintilla::Message::IndicSetOutlineAlpha), findIndicator, 160);
    send(message(Scintilla::Message::IndicSetUnder), findIndicator, 1);
    send(message(Scintilla::Message::IndicSetStyle), selectionIndicator,
         static_cast<sptr_t>(Scintilla::IndicatorStyle::RoundBox));
    send(message(Scintilla::Message::IndicSetAlpha), selectionIndicator, 40);
    send(message(Scintilla::Message::IndicSetOutlineAlpha), selectionIndicator, 110);
    send(message(Scintilla::Message::IndicSetUnder), selectionIndicator, 1);
    applyViewOptions();
}

bool EditorWidget::applyViewOptions() {
    const bool fullFeatures = !largeFileMode_;
    if (send(message(Scintilla::Message::GetZoom)) != viewOptions_.zoom) {
        send(message(Scintilla::Message::SetZoom), static_cast<uptr_t>(viewOptions_.zoom));
        lineNumberDigits_ = 0;
        updateLineNumberMarginWidth();
    }
    send(message(Scintilla::Message::SetWrapMode),
         static_cast<uptr_t>(viewOptions_.wordWrap && fullFeatures ? Scintilla::Wrap::Word
                                                                   : Scintilla::Wrap::None));
    send(message(Scintilla::Message::SetViewWS),
         static_cast<uptr_t>(viewOptions_.showWhitespace ? Scintilla::WhiteSpace::VisibleAlways
                                                         : Scintilla::WhiteSpace::Invisible));
    send(message(Scintilla::Message::SetViewEOL), viewOptions_.showWhitespace ? 1 : 0);
    send(message(Scintilla::Message::SetIndentationGuides),
         static_cast<uptr_t>(viewOptions_.indentGuides ? Scintilla::IndentView::LookBoth
                                                       : Scintilla::IndentView::None));
    send(message(Scintilla::Message::SetEdgeColumn), static_cast<uptr_t>(viewOptions_.rulerColumn));
    send(message(Scintilla::Message::SetEdgeMode),
         static_cast<uptr_t>(viewOptions_.showRuler ? Scintilla::EdgeVisualStyle::Line
                                                    : Scintilla::EdgeVisualStyle::None));
    send(message(Scintilla::Message::SetTabWidth), static_cast<uptr_t>(viewOptions_.tabWidth));
    send(message(Scintilla::Message::SetUseTabs), viewOptions_.useTabs ? 1 : 0);

    const bool folding = viewOptions_.codeFolding && fullFeatures && !hibernated_ &&
                         !lexerName_.isEmpty() && lexerName_ != QStringLiteral("null");
    send(message(Scintilla::Message::SetMarginWidthN), foldMargin, folding ? symbolMarginWidth : 0);
    if (folding == foldingEnabled_) {
        return false;
    }

    if (!folding) {
        setAllFoldsExpanded(true);
    }
    foldingEnabled_ = folding;
    const auto setProperty = [this](const char* key, const char* value) {
        send(message(Scintilla::Message::SetProperty), reinterpret_cast<uptr_t>(key),
             reinterpret_cast<sptr_t>(value));
    };
    const char* enabled = folding ? "1" : "0";
    setProperty("fold", enabled);
    setProperty("fold.comment", enabled);
    setProperty("fold.html", enabled);
    setProperty("fold.compact", "0");
    return true;
}

void EditorWidget::updateBraceHighlight() {
    sptr_t brace = -1;
    sptr_t match = -1;
    if (!largeFileMode_ && !hibernated_) {
        const auto caret = send(message(Scintilla::Message::GetCurrentPos));
        if (caret > 0 && isBrace(charAt(caret - 1))) {
            brace = caret - 1;
        } else if (isBrace(charAt(caret))) {
            brace = caret;
        }
        if (brace >= 0) {
            match = send(message(Scintilla::Message::BraceMatch), static_cast<uptr_t>(brace), 0);
        }
    }
    if (brace == highlightedBrace_ && match == highlightedBraceMatch_) {
        return;
    }

    highlightedBrace_ = brace;
    highlightedBraceMatch_ = match;
    if (brace >= 0 && match < 0) {
        send(message(Scintilla::Message::BraceBadLight), static_cast<uptr_t>(brace));
    } else {
        send(message(Scintilla::Message::BraceHighlight), static_cast<uptr_t>(brace), match);
    }
}

void EditorWidget::handleCharAdded(const int character) {
    if (internalMutation_) {
        return;
    }

    const bool lineBreak =
        character == '\n' ||
        (character == '\r' && send(message(Scintilla::Message::GetEOLMode)) == SC_EOL_CR);
    if (lineBreak) {
        if (viewOptions_.autoIndent) {
            autoIndentCurrentLine();
        }
        return;
    }
    if (viewOptions_.wordCompletion && isWordCharacter(character) &&
        send(message(Scintilla::Message::AutoCActive)) == 0) {
        showWordCompletions(false);
    }
    if (!viewOptions_.autoCloseBrackets) {
        return;
    }

    const auto caret = send(message(Scintilla::Message::GetCurrentPos));
    const int next = charAt(caret);
    const bool isQuote = character == '"' || character == '\'' || character == '`';
    const bool isClosingBracket = character == ')' || character == ']' || character == '}';
    if ((isQuote || isClosingBracket) && next == character) {
        // Type over the closing character instead of doubling it.
        send(message(Scintilla::Message::DeleteRange), static_cast<uptr_t>(caret), 1);
        return;
    }

    char closing = '\0';
    switch (character) {
    case '(':
        closing = ')';
        break;
    case '[':
        closing = ']';
        break;
    case '{':
        closing = '}';
        break;
    default:
        closing = isQuote ? static_cast<char>(character) : '\0';
        break;
    }
    if (closing == '\0' || isWordCharacter(next)) {
        return;
    }
    if (isQuote && caret >= 2 && isWordCharacter(charAt(caret - 2))) {
        // Leave apostrophes in words such as "don't" alone.
        return;
    }

    const char text[] = {closing, '\0'};
    sends(message(Scintilla::Message::InsertText), static_cast<uptr_t>(caret), text);
}

void EditorWidget::autoIndentCurrentLine() {
    const auto caret = send(message(Scintilla::Message::GetCurrentPos));
    const auto line =
        send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(caret));
    if (line == 0) {
        return;
    }

    auto indentation =
        send(message(Scintilla::Message::GetLineIndentation), static_cast<uptr_t>(line - 1));
    const QByteArray previous = linesInRange({line - 1, line - 1}).first().trimmed();
    if (!previous.isEmpty()) {
        const char last = previous.back();
        if (last == '{' || last == '[' || last == '(' ||
            (last == ':' && syntaxName_ == QStringLiteral("python"))) {
            indentation += viewOptions_.tabWidth;
        }
    }
    if (indentation <= 0) {
        return;
    }

    send(message(Scintilla::Message::SetLineIndentation), static_cast<uptr_t>(line), indentation);
    const auto indentPosition =
        send(message(Scintilla::Message::GetLineIndentPosition), static_cast<uptr_t>(line));
    send(message(Scintilla::Message::SetEmptySelection), static_cast<uptr_t>(indentPosition));
}

void EditorWidget::toggleBookmarkAtLine(const sptr_t line) {
    if ((send(message(Scintilla::Message::MarkerGet), static_cast<uptr_t>(line)) & bookmarkMask) !=
        0) {
        send(message(Scintilla::Message::MarkerDelete), static_cast<uptr_t>(line), bookmarkMarker);
    } else {
        send(message(Scintilla::Message::MarkerAdd), static_cast<uptr_t>(line), bookmarkMarker);
    }
}

int EditorWidget::charAt(const sptr_t position) const {
    return static_cast<int>(
        send(message(Scintilla::Message::GetCharAt), static_cast<uptr_t>(position)));
}

QByteArray EditorWidget::textRange(const sptr_t start, const sptr_t end) const {
    if (end <= start) {
        return {};
    }
    QByteArray value(static_cast<qsizetype>(end - start) + 1, '\0');
    Sci_TextRangeFull range{};
    range.chrg.cpMin = start;
    range.chrg.cpMax = end;
    range.lpstrText = value.data();
    send(message(Scintilla::Message::GetTextRangeFull), 0, reinterpret_cast<sptr_t>(&range));
    value.chop(1);
    return value;
}

QByteArray EditorWidget::endOfLine() const {
    switch (send(message(Scintilla::Message::GetEOLMode))) {
    case SC_EOL_CRLF:
        return QByteArrayLiteral("\r\n");
    case SC_EOL_CR:
        return QByteArrayLiteral("\r");
    default:
        return QByteArrayLiteral("\n");
    }
}

EditorWidget::LineRange EditorWidget::selectedLineRange(const bool wholeDocumentWhenEmpty) const {
    const auto selectionStart = send(message(Scintilla::Message::GetSelectionStart));
    const auto selectionEnd = send(message(Scintilla::Message::GetSelectionEnd));
    if (selectionStart == selectionEnd && wholeDocumentWhenEmpty) {
        return {0, send(message(Scintilla::Message::GetLineCount)) - 1};
    }

    const auto first =
        send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(selectionStart));
    auto last =
        send(message(Scintilla::Message::LineFromPosition), static_cast<uptr_t>(selectionEnd));
    if (last > first && selectionEnd == send(message(Scintilla::Message::PositionFromLine),
                                             static_cast<uptr_t>(last))) {
        // A selection ending at column 0 does not include that line.
        --last;
    }
    return {first, last};
}

QList<QByteArray> EditorWidget::linesInRange(const LineRange range) const {
    QList<QByteArray> lines;
    lines.reserve(static_cast<qsizetype>(range.last - range.first + 1));
    for (sptr_t line = range.first; line <= range.last; ++line) {
        lines.append(textRange(
            send(message(Scintilla::Message::PositionFromLine), static_cast<uptr_t>(line)),
            send(message(Scintilla::Message::GetLineEndPosition), static_cast<uptr_t>(line))));
    }
    return lines;
}

void EditorWidget::replaceLines(const LineRange range, const QList<QByteArray>& lines) {
    const auto start =
        send(message(Scintilla::Message::PositionFromLine), static_cast<uptr_t>(range.first));
    const auto end =
        send(message(Scintilla::Message::GetLineEndPosition), static_cast<uptr_t>(range.last));
    const QByteArray replacement = lines.join(endOfLine());
    if (replacement == textRange(start, end)) {
        return;
    }

    send(message(Scintilla::Message::SetTargetStart), static_cast<uptr_t>(start));
    send(message(Scintilla::Message::SetTargetEnd), static_cast<uptr_t>(end));
    sends(message(Scintilla::Message::ReplaceTarget), static_cast<uptr_t>(replacement.size()),
          replacement.constData());
    send(message(Scintilla::Message::SetSel), static_cast<uptr_t>(start),
         start + replacement.size());
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

void EditorWidget::setSearchOptions(const SearchOptions& searchOptions) {
    int options = static_cast<int>(Scintilla::FindOption::None);
    if (searchOptions.matchCase) {
        options |= static_cast<int>(Scintilla::FindOption::MatchCase);
    }
    if (searchOptions.wholeWord) {
        options |= static_cast<int>(Scintilla::FindOption::WholeWord);
    }
    if (searchOptions.regex) {
        options |= static_cast<int>(Scintilla::FindOption::RegExp) |
                   static_cast<int>(Scintilla::FindOption::Cxx11RegEx);
    }
    send(message(Scintilla::Message::SetSearchFlags), static_cast<uptr_t>(options));
}

std::pair<sptr_t, sptr_t> EditorWidget::searchBounds(const SearchOptions& options) const {
    const auto length = send(message(Scintilla::Message::GetLength));
    if (options.inSelection && searchScopeActive_) {
        return {qMin(searchScopeStart_, length), qMin(searchScopeEnd_, length)};
    }
    return {0, length};
}

sptr_t EditorWidget::replaceTarget(const QByteArray& replacement, const bool regex) {
    return sends(
        message(regex ? Scintilla::Message::ReplaceTargetRE : Scintilla::Message::ReplaceTarget),
        static_cast<uptr_t>(replacement.size()), replacement.constData());
}

void EditorWidget::adjustSearchScope(const sptr_t delta) {
    if (searchScopeActive_) {
        searchScopeEnd_ = qMax(searchScopeStart_, searchScopeEnd_ + delta);
    }
}

bool EditorWidget::selectionMatches(const QByteArray& query, const SearchOptions& options) {
    const auto selectionStart = send(message(Scintilla::Message::GetSelectionStart));
    const auto selectionEnd = send(message(Scintilla::Message::GetSelectionEnd));
    if (selectionStart == selectionEnd) {
        return false;
    }

    setSearchOptions(options);
    send(message(Scintilla::Message::SetTargetStart), static_cast<uptr_t>(selectionStart));
    send(message(Scintilla::Message::SetTargetEnd), static_cast<uptr_t>(selectionEnd));
    const auto match = sends(message(Scintilla::Message::SearchInTarget),
                             static_cast<uptr_t>(query.size()), query.constData());
    return match == selectionStart &&
           send(message(Scintilla::Message::GetTargetEnd)) == selectionEnd;
}

void EditorWidget::applyLexerTheme(const ThemePalette& palette) {
    const QString& keyword =
        palette.syntaxKeyword.isEmpty() ? palette.accent : palette.syntaxKeyword;
    const QString& string =
        palette.syntaxString.isEmpty() ? palette.positive : palette.syntaxString;
    const QString& number = palette.syntaxNumber.isEmpty() ? palette.info : palette.syntaxNumber;
    const QString& type = palette.syntaxType.isEmpty() ? palette.info : palette.syntaxType;
    const QString& comment =
        palette.syntaxComment.isEmpty() ? palette.textMuted : palette.syntaxComment;
    const QString& attribute =
        palette.syntaxAttribute.isEmpty() ? palette.warning : palette.syntaxAttribute;

    if (lexerName_ == QStringLiteral("cpp")) {
        setStyles(*this,
                  {SCE_C_COMMENT, SCE_C_COMMENTLINE, SCE_C_COMMENTDOC, SCE_C_COMMENTLINEDOC,
                   SCE_C_PREPROCESSORCOMMENT, SCE_C_PREPROCESSORCOMMENTDOC},
                  comment, false, true);
        applyStyle(*this, SCE_C_NUMBER, number);
        applyStyle(*this, SCE_C_WORD, keyword, true);
        setStyles(*this,
                  {SCE_C_STRING, SCE_C_CHARACTER, SCE_C_STRINGEOL, SCE_C_VERBATIM, SCE_C_REGEX,
                   SCE_C_STRINGRAW, SCE_C_TRIPLEVERBATIM, SCE_C_HASHQUOTEDSTRING, SCE_C_USERLITERAL,
                   SCE_C_ESCAPESEQUENCE},
                  string);
        applyStyle(*this, SCE_C_PREPROCESSOR, attribute);
        applyStyle(*this, SCE_C_WORD2, type, true);
        applyStyle(*this, SCE_C_COMMENTDOCKEYWORD, keyword);
        applyStyle(*this, SCE_C_COMMENTDOCKEYWORDERROR, palette.danger);
        applyStyle(*this, SCE_C_GLOBALCLASS, type);
    } else if (lexerName_ == QStringLiteral("python")) {
        setStyles(*this, {SCE_P_COMMENTLINE, SCE_P_COMMENTBLOCK}, comment, false, true);
        applyStyle(*this, SCE_P_NUMBER, number);
        setStyles(*this,
                  {SCE_P_STRING, SCE_P_CHARACTER, SCE_P_TRIPLE, SCE_P_TRIPLEDOUBLE, SCE_P_STRINGEOL,
                   SCE_P_FSTRING, SCE_P_FCHARACTER, SCE_P_FTRIPLE, SCE_P_FTRIPLEDOUBLE},
                  string);
        setStyles(*this, {SCE_P_WORD, SCE_P_WORD2}, keyword, true);
        setStyles(*this, {SCE_P_CLASSNAME, SCE_P_DEFNAME}, type, true);
        applyStyle(*this, SCE_P_DECORATOR, attribute);
        applyStyle(*this, SCE_P_ATTRIBUTE, type);
    } else if (lexerName_ == QStringLiteral("hypertext") || lexerName_ == QStringLiteral("xml") ||
               lexerName_ == QStringLiteral("phpscript")) {
        setStyles(*this, {SCE_H_TAG, SCE_H_TAGEND, SCE_H_XMLSTART, SCE_H_XMLEND}, keyword, true);
        setStyles(*this, {SCE_H_ATTRIBUTE, SCE_H_ENTITY}, type);
        setStyles(*this, {SCE_H_DOUBLESTRING, SCE_H_SINGLESTRING, SCE_H_VALUE}, string);
        setStyles(*this, {SCE_H_COMMENT, SCE_H_XCCOMMENT, SCE_H_SGML_COMMENT}, comment, false,
                  true);
        applyStyle(*this, SCE_H_NUMBER, number);
        setStyles(*this, {SCE_H_TAGUNKNOWN, SCE_H_ATTRIBUTEUNKNOWN, SCE_H_SGML_ERROR},
                  palette.danger);
        setStyles(*this, {SCE_HJ_COMMENT, SCE_HJ_COMMENTLINE, SCE_HJ_COMMENTDOC}, comment, false,
                  true);
        applyStyle(*this, SCE_HJ_NUMBER, number);
        setStyles(*this, {SCE_HJ_WORD, SCE_HJ_KEYWORD}, keyword, true);
        setStyles(*this,
                  {SCE_HJ_DOUBLESTRING, SCE_HJ_SINGLESTRING, SCE_HJ_REGEX, SCE_HJ_TEMPLATELITERAL},
                  string);
        setStyles(*this, {SCE_HPHP_COMMENT, SCE_HPHP_COMMENTLINE}, comment, false, true);
        applyStyle(*this, SCE_HPHP_WORD, keyword, true);
        applyStyle(*this, SCE_HPHP_NUMBER, number);
        setStyles(*this, {SCE_HPHP_HSTRING, SCE_HPHP_SIMPLESTRING, SCE_HPHP_HSTRING_VARIABLE},
                  string);
        setStyles(*this, {SCE_HPHP_VARIABLE, SCE_HPHP_COMPLEX_VARIABLE}, attribute);
    } else if (lexerName_ == QStringLiteral("json")) {
        applyStyle(*this, SCE_JSON_NUMBER, number);
        setStyles(*this, {SCE_JSON_STRING, SCE_JSON_URI, SCE_JSON_COMPACTIRI}, string);
        applyStyle(*this, SCE_JSON_PROPERTYNAME, keyword);
        setStyles(*this, {SCE_JSON_LINECOMMENT, SCE_JSON_BLOCKCOMMENT}, comment, false, true);
        setStyles(*this, {SCE_JSON_KEYWORD, SCE_JSON_LDKEYWORD}, keyword, true);
        setStyles(*this, {SCE_JSON_STRINGEOL, SCE_JSON_ERROR}, palette.danger);
    } else if (lexerName_ == QStringLiteral("markdown")) {
        setStyles(*this,
                  {SCE_MARKDOWN_HEADER1, SCE_MARKDOWN_HEADER2, SCE_MARKDOWN_HEADER3,
                   SCE_MARKDOWN_HEADER4, SCE_MARKDOWN_HEADER5, SCE_MARKDOWN_HEADER6},
                  keyword, true);
        setStyles(*this, {SCE_MARKDOWN_STRONG1, SCE_MARKDOWN_STRONG2}, palette.textMain, true);
        setStyles(*this, {SCE_MARKDOWN_EM1, SCE_MARKDOWN_EM2}, palette.textSecondary, false, true);
        setStyles(*this, {SCE_MARKDOWN_CODE, SCE_MARKDOWN_CODE2, SCE_MARKDOWN_CODEBK}, string);
        applyStyle(*this, SCE_MARKDOWN_LINK, type);
        applyStyle(*this, SCE_MARKDOWN_BLOCKQUOTE, comment, false, true);
        setStyles(*this, {SCE_MARKDOWN_ULIST_ITEM, SCE_MARKDOWN_OLIST_ITEM}, attribute);
    } else if (lexerName_ == QStringLiteral("rust")) {
        setStyles(*this,
                  {SCE_RUST_COMMENTBLOCK, SCE_RUST_COMMENTLINE, SCE_RUST_COMMENTBLOCKDOC,
                   SCE_RUST_COMMENTLINEDOC},
                  comment, false, true);
        applyStyle(*this, SCE_RUST_NUMBER, number);
        setStyles(*this,
                  {SCE_RUST_WORD, SCE_RUST_WORD2, SCE_RUST_WORD3, SCE_RUST_WORD4, SCE_RUST_WORD5,
                   SCE_RUST_WORD6, SCE_RUST_WORD7},
                  keyword, true);
        setStyles(*this,
                  {SCE_RUST_STRING, SCE_RUST_STRINGR, SCE_RUST_CHARACTER, SCE_RUST_BYTESTRING,
                   SCE_RUST_BYTESTRINGR, SCE_RUST_BYTECHARACTER, SCE_RUST_CSTRING,
                   SCE_RUST_CSTRINGR},
                  string);
        setStyles(*this, {SCE_RUST_LIFETIME, SCE_RUST_MACRO}, attribute);
        applyStyle(*this, SCE_RUST_LEXERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("css")) {
        applyStyle(*this, SCE_CSS_COMMENT, comment, false, true);
        setStyles(*this, {SCE_CSS_TAG, SCE_CSS_CLASS, SCE_CSS_ID, SCE_CSS_ATTRIBUTE}, keyword);
        setStyles(*this,
                  {SCE_CSS_PSEUDOCLASS, SCE_CSS_PSEUDOELEMENT, SCE_CSS_EXTENDED_PSEUDOCLASS,
                   SCE_CSS_EXTENDED_PSEUDOELEMENT},
                  type);
        setStyles(*this,
                  {SCE_CSS_IDENTIFIER, SCE_CSS_IDENTIFIER2, SCE_CSS_IDENTIFIER3,
                   SCE_CSS_EXTENDED_IDENTIFIER, SCE_CSS_VARIABLE},
                  palette.textSecondary);
        setStyles(*this, {SCE_CSS_DOUBLESTRING, SCE_CSS_SINGLESTRING}, string);
        setStyles(*this, {SCE_CSS_IMPORTANT, SCE_CSS_DIRECTIVE, SCE_CSS_GROUP_RULE}, attribute,
                  true);
        setStyles(*this, {SCE_CSS_UNKNOWN_IDENTIFIER, SCE_CSS_UNKNOWN_PSEUDOCLASS}, palette.danger);
    } else if (lexerName_ == QStringLiteral("bash")) {
        applyStyle(*this, SCE_SH_COMMENTLINE, comment, false, true);
        applyStyle(*this, SCE_SH_WORD, keyword, true);
        applyStyle(*this, SCE_SH_NUMBER, number);
        setStyles(
            *this,
            {SCE_SH_STRING, SCE_SH_CHARACTER, SCE_SH_BACKTICKS, SCE_SH_HERE_DELIM, SCE_SH_HERE_Q},
            string);
        setStyles(*this, {SCE_SH_SCALAR, SCE_SH_PARAM}, attribute);
        applyStyle(*this, SCE_SH_ERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("yaml")) {
        applyStyle(*this, SCE_YAML_COMMENT, comment, false, true);
        applyStyle(*this, SCE_YAML_IDENTIFIER, keyword);
        setStyles(*this, {SCE_YAML_DEFAULT, SCE_YAML_TEXT}, palette.textSecondary);
        applyStyle(*this, SCE_YAML_KEYWORD, keyword, true);
        applyStyle(*this, SCE_YAML_NUMBER, number);
        setStyles(*this, {SCE_YAML_REFERENCE, SCE_YAML_DOCUMENT}, string);
        applyStyle(*this, SCE_YAML_ERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("toml")) {
        applyStyle(*this, SCE_TOML_COMMENT, comment, false, true);
        setStyles(*this, {SCE_TOML_IDENTIFIER, SCE_TOML_KEY, SCE_TOML_TABLE}, keyword);
        applyStyle(*this, SCE_TOML_KEYWORD, attribute, true);
        setStyles(*this, {SCE_TOML_NUMBER, SCE_TOML_DATETIME}, number);
        setStyles(*this,
                  {SCE_TOML_STRING_SQ, SCE_TOML_STRING_DQ, SCE_TOML_TRIPLE_STRING_SQ,
                   SCE_TOML_TRIPLE_STRING_DQ, SCE_TOML_ESCAPECHAR},
                  string);
        setStyles(*this, {SCE_TOML_ERROR, SCE_TOML_STRINGEOL}, palette.danger);
    } else if (lexerName_ == QStringLiteral("sql")) {
        setStyles(
            *this,
            {SCE_SQL_COMMENT, SCE_SQL_COMMENTLINE, SCE_SQL_COMMENTDOC, SCE_SQL_COMMENTLINEDOC},
            comment, false, true);
        applyStyle(*this, SCE_SQL_NUMBER, number);
        setStyles(*this, {SCE_SQL_WORD, SCE_SQL_WORD2}, keyword, true);
        setStyles(*this, {SCE_SQL_STRING, SCE_SQL_CHARACTER}, string);
        applyStyle(*this, SCE_SQL_QUOTEDIDENTIFIER, attribute);
        applyStyle(*this, SCE_SQL_COMMENTDOCKEYWORDERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("ruby")) {
        setStyles(*this, {SCE_RB_COMMENTLINE, SCE_RB_POD}, comment, false, true);
        applyStyle(*this, SCE_RB_WORD, keyword, true);
        applyStyle(*this, SCE_RB_NUMBER, number);
        setStyles(*this,
                  {SCE_RB_STRING, SCE_RB_CHARACTER, SCE_RB_REGEX, SCE_RB_SYMBOL, SCE_RB_BACKTICKS,
                   SCE_RB_HERE_Q, SCE_RB_HERE_QQ, SCE_RB_HERE_QX},
                  string);
        setStyles(*this, {SCE_RB_CLASSNAME, SCE_RB_DEFNAME, SCE_RB_MODULE_NAME}, type, true);
        setStyles(*this, {SCE_RB_GLOBAL, SCE_RB_INSTANCE_VAR, SCE_RB_CLASS_VAR}, attribute);
        applyStyle(*this, SCE_RB_ERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("lua")) {
        setStyles(*this, {SCE_LUA_COMMENT, SCE_LUA_COMMENTLINE, SCE_LUA_COMMENTDOC}, comment, false,
                  true);
        applyStyle(*this, SCE_LUA_WORD, keyword, true);
        applyStyle(*this, SCE_LUA_NUMBER, number);
        setStyles(*this, {SCE_LUA_STRING, SCE_LUA_CHARACTER, SCE_LUA_LITERALSTRING}, string);
        setStyles(*this, {SCE_LUA_WORD2, SCE_LUA_WORD3, SCE_LUA_WORD4}, type);
        applyStyle(*this, SCE_LUA_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("dart")) {
        setStyles(*this,
                  {SCE_DART_COMMENTLINE, SCE_DART_COMMENTLINEDOC, SCE_DART_COMMENTBLOCK,
                   SCE_DART_COMMENTBLOCKDOC},
                  comment, false, true);
        setStyles(
            *this,
            {SCE_DART_KW_PRIMARY, SCE_DART_KW_SECONDARY, SCE_DART_KW_TERTIARY, SCE_DART_KW_TYPE},
            keyword, true);
        applyStyle(*this, SCE_DART_NUMBER, number);
        setStyles(*this,
                  {SCE_DART_STRING_SQ, SCE_DART_STRING_DQ, SCE_DART_TRIPLE_STRING_SQ,
                   SCE_DART_TRIPLE_STRING_DQ, SCE_DART_RAWSTRING_SQ, SCE_DART_RAWSTRING_DQ,
                   SCE_DART_ESCAPECHAR},
                  string);
        applyStyle(*this, SCE_DART_METADATA, attribute);
        applyStyle(*this, SCE_DART_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("zig")) {
        setStyles(*this, {SCE_ZIG_COMMENTLINE, SCE_ZIG_COMMENTLINEDOC, SCE_ZIG_COMMENTLINETOP},
                  comment, false, true);
        setStyles(*this,
                  {SCE_ZIG_KW_PRIMARY, SCE_ZIG_KW_SECONDARY, SCE_ZIG_KW_TERTIARY, SCE_ZIG_KW_TYPE},
                  keyword, true);
        applyStyle(*this, SCE_ZIG_NUMBER, number);
        setStyles(*this,
                  {SCE_ZIG_CHARACTER, SCE_ZIG_STRING, SCE_ZIG_MULTISTRING, SCE_ZIG_ESCAPECHAR},
                  string);
        setStyles(*this, {SCE_ZIG_FUNCTION, SCE_ZIG_BUILTIN_FUNCTION}, type);
        applyStyle(*this, SCE_ZIG_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("props")) {
        applyStyle(*this, SCE_PROPS_COMMENT, comment, false, true);
        applyStyle(*this, SCE_PROPS_SECTION, keyword, true);
        applyStyle(*this, SCE_PROPS_KEY, type);
        applyStyle(*this, SCE_PROPS_ASSIGNMENT, attribute);
        applyStyle(*this, SCE_PROPS_DEFVAL, string);
    } else if (lexerName_ == QStringLiteral("diff")) {
        setStyles(*this, {SCE_DIFF_COMMENT, SCE_DIFF_COMMAND}, comment);
        setStyles(*this, {SCE_DIFF_HEADER, SCE_DIFF_POSITION}, keyword, true);
        setStyles(*this, {SCE_DIFF_ADDED, SCE_DIFF_PATCH_ADD}, palette.positive);
        setStyles(*this,
                  {SCE_DIFF_DELETED, SCE_DIFF_PATCH_DELETE, SCE_DIFF_REMOVED_PATCH_ADD,
                   SCE_DIFF_REMOVED_PATCH_DELETE},
                  palette.danger);
        applyStyle(*this, SCE_DIFF_CHANGED, palette.warning);
    } else if (lexerName_ == QStringLiteral("makefile")) {
        applyStyle(*this, SCE_MAKE_COMMENT, comment, false, true);
        applyStyle(*this, SCE_MAKE_PREPROCESSOR, attribute);
        applyStyle(*this, SCE_MAKE_IDENTIFIER, type);
        applyStyle(*this, SCE_MAKE_TARGET, keyword, true);
        applyStyle(*this, SCE_MAKE_IDEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("cmake")) {
        applyStyle(*this, SCE_CMAKE_COMMENT, comment, false, true);
        setStyles(*this,
                  {SCE_CMAKE_STRINGDQ, SCE_CMAKE_STRINGLQ, SCE_CMAKE_STRINGRQ, SCE_CMAKE_STRINGVAR},
                  string);
        setStyles(*this,
                  {SCE_CMAKE_COMMANDS, SCE_CMAKE_WHILEDEF, SCE_CMAKE_FOREACHDEF,
                   SCE_CMAKE_IFDEFINEDEF, SCE_CMAKE_MACRODEF},
                  keyword, true);
        applyStyle(*this, SCE_CMAKE_PARAMETERS, type);
        applyStyle(*this, SCE_CMAKE_VARIABLE, attribute);
        applyStyle(*this, SCE_CMAKE_NUMBER, number);
    } else if (lexerName_ == QStringLiteral("perl")) {
        setStyles(*this, {SCE_PL_COMMENTLINE, SCE_PL_POD, SCE_PL_POD_VERB}, comment, false, true);
        applyStyle(*this, SCE_PL_WORD, keyword, true);
        applyStyle(*this, SCE_PL_NUMBER, number);
        setStyles(*this,
                  {SCE_PL_STRING, SCE_PL_CHARACTER, SCE_PL_STRING_Q, SCE_PL_STRING_QQ,
                   SCE_PL_STRING_QW, SCE_PL_HERE_Q, SCE_PL_HERE_QQ, SCE_PL_BACKTICKS},
                  string);
        setStyles(*this, {SCE_PL_SCALAR, SCE_PL_ARRAY, SCE_PL_HASH, SCE_PL_SYMBOLTABLE}, attribute);
        setStyles(*this, {SCE_PL_REGEX, SCE_PL_REGSUBST, SCE_PL_STRING_QR}, type);
        applyStyle(*this, SCE_PL_ERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("r")) {
        applyStyle(*this, SCE_R_COMMENT, comment, false, true);
        setStyles(*this, {SCE_R_KWORD, SCE_R_BASEKWORD}, keyword, true);
        applyStyle(*this, SCE_R_OTHERKWORD, type);
        applyStyle(*this, SCE_R_NUMBER, number);
        setStyles(
            *this,
            {SCE_R_STRING, SCE_R_STRING2, SCE_R_RAWSTRING, SCE_R_RAWSTRING2, SCE_R_ESCAPESEQUENCE},
            string);
        setStyles(*this, {SCE_R_INFIX, SCE_R_BACKTICKS}, attribute);
    } else if (lexerName_ == QStringLiteral("batch")) {
        applyStyle(*this, SCE_BAT_COMMENT, comment, false, true);
        applyStyle(*this, SCE_BAT_WORD, keyword, true);
        applyStyle(*this, SCE_BAT_LABEL, type, true);
        setStyles(*this, {SCE_BAT_HIDE, SCE_BAT_COMMAND}, type);
        applyStyle(*this, SCE_BAT_IDENTIFIER, attribute);
    } else if (lexerName_ == QStringLiteral("powershell")) {
        setStyles(*this, {SCE_POWERSHELL_COMMENT, SCE_POWERSHELL_COMMENTSTREAM}, comment, false,
                  true);
        applyStyle(*this, SCE_POWERSHELL_KEYWORD, keyword, true);
        setStyles(*this, {SCE_POWERSHELL_CMDLET, SCE_POWERSHELL_ALIAS, SCE_POWERSHELL_FUNCTION},
                  type);
        applyStyle(*this, SCE_POWERSHELL_NUMBER, number);
        setStyles(*this,
                  {SCE_POWERSHELL_STRING, SCE_POWERSHELL_CHARACTER, SCE_POWERSHELL_HERE_STRING,
                   SCE_POWERSHELL_HERE_CHARACTER},
                  string);
        applyStyle(*this, SCE_POWERSHELL_VARIABLE, attribute);
    } else if (lexerName_ == QStringLiteral("haskell")) {
        setStyles(
            *this,
            {SCE_HA_COMMENTLINE, SCE_HA_COMMENTBLOCK, SCE_HA_COMMENTBLOCK2, SCE_HA_COMMENTBLOCK3},
            comment, false, true);
        setStyles(*this, {SCE_HA_KEYWORD, SCE_HA_IMPORT}, keyword, true);
        applyStyle(*this, SCE_HA_NUMBER, number);
        setStyles(*this, {SCE_HA_STRING, SCE_HA_CHARACTER}, string);
        setStyles(*this,
                  {SCE_HA_CLASS, SCE_HA_MODULE, SCE_HA_CAPITAL, SCE_HA_DATA, SCE_HA_INSTANCE},
                  type);
        setStyles(*this, {SCE_HA_PRAGMA, SCE_HA_PREPROCESSOR}, attribute);
        applyStyle(*this, SCE_HA_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("erlang")) {
        setStyles(*this,
                  {SCE_ERLANG_COMMENT, SCE_ERLANG_COMMENT_FUNCTION, SCE_ERLANG_COMMENT_MODULE,
                   SCE_ERLANG_COMMENT_DOC},
                  comment, false, true);
        applyStyle(*this, SCE_ERLANG_KEYWORD, keyword, true);
        applyStyle(*this, SCE_ERLANG_NUMBER, number);
        setStyles(*this, {SCE_ERLANG_STRING, SCE_ERLANG_CHARACTER}, string);
        setStyles(*this, {SCE_ERLANG_FUNCTION_NAME, SCE_ERLANG_BIFS, SCE_ERLANG_MODULES}, type);
        setStyles(*this,
                  {SCE_ERLANG_VARIABLE, SCE_ERLANG_MACRO, SCE_ERLANG_RECORD, SCE_ERLANG_PREPROC},
                  attribute);
        applyStyle(*this, SCE_ERLANG_UNKNOWN, palette.danger);
    } else if (lexerName_ == QStringLiteral("lisp")) {
        setStyles(*this, {SCE_LISP_COMMENT, SCE_LISP_MULTI_COMMENT}, comment, false, true);
        setStyles(*this, {SCE_LISP_KEYWORD, SCE_LISP_KEYWORD_KW}, keyword, true);
        applyStyle(*this, SCE_LISP_NUMBER, number);
        applyStyle(*this, SCE_LISP_STRING, string);
        setStyles(*this, {SCE_LISP_SYMBOL, SCE_LISP_SPECIAL}, attribute);
        applyStyle(*this, SCE_LISP_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("pascal")) {
        setStyles(*this, {SCE_PAS_COMMENT, SCE_PAS_COMMENT2, SCE_PAS_COMMENTLINE}, comment, false,
                  true);
        applyStyle(*this, SCE_PAS_WORD, keyword, true);
        setStyles(*this, {SCE_PAS_NUMBER, SCE_PAS_HEXNUMBER}, number);
        setStyles(*this, {SCE_PAS_STRING, SCE_PAS_CHARACTER, SCE_PAS_MULTILINESTRING}, string);
        setStyles(*this, {SCE_PAS_PREPROCESSOR, SCE_PAS_PREPROCESSOR2}, attribute);
        applyStyle(*this, SCE_PAS_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("fortran")) {
        applyStyle(*this, SCE_F_COMMENT, comment, false, true);
        applyStyle(*this, SCE_F_WORD, keyword, true);
        setStyles(*this, {SCE_F_WORD2, SCE_F_WORD3}, type);
        applyStyle(*this, SCE_F_NUMBER, number);
        setStyles(*this, {SCE_F_STRING1, SCE_F_STRING2}, string);
        setStyles(*this, {SCE_F_PREPROCESSOR, SCE_F_LABEL}, attribute);
        applyStyle(*this, SCE_F_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("tcl")) {
        setStyles(
            *this,
            {SCE_TCL_COMMENT, SCE_TCL_COMMENTLINE, SCE_TCL_COMMENT_BOX, SCE_TCL_BLOCK_COMMENT},
            comment, false, true);
        setStyles(*this, {SCE_TCL_WORD, SCE_TCL_WORD2, SCE_TCL_WORD3}, keyword, true);
        applyStyle(*this, SCE_TCL_NUMBER, number);
        setStyles(*this, {SCE_TCL_IN_QUOTE, SCE_TCL_WORD_IN_QUOTE}, string);
        setStyles(*this, {SCE_TCL_SUBSTITUTION, SCE_TCL_SUB_BRACE}, attribute);
        applyStyle(*this, SCE_TCL_MODIFIER, type);
    } else if (lexerName_ == QStringLiteral("vb")) {
        setStyles(*this, {SCE_B_COMMENT, SCE_B_COMMENTBLOCK, SCE_B_DOCLINE, SCE_B_DOCBLOCK},
                  comment, false, true);
        setStyles(*this, {SCE_B_KEYWORD, SCE_B_KEYWORD2}, keyword, true);
        setStyles(*this, {SCE_B_KEYWORD3, SCE_B_KEYWORD4, SCE_B_CONSTANT}, type);
        setStyles(*this, {SCE_B_NUMBER, SCE_B_HEXNUMBER, SCE_B_BINNUMBER, SCE_B_DATE}, type);
        applyStyle(*this, SCE_B_STRING, string);
        setStyles(*this, {SCE_B_PREPROCESSOR, SCE_B_LABEL}, attribute);
        setStyles(*this, {SCE_B_STRINGEOL, SCE_B_ERROR}, palette.danger);
    } else if (lexerName_ == QStringLiteral("d")) {
        setStyles(*this,
                  {SCE_D_COMMENT, SCE_D_COMMENTLINE, SCE_D_COMMENTDOC, SCE_D_COMMENTNESTED,
                   SCE_D_COMMENTLINEDOC},
                  comment, false, true);
        applyStyle(*this, SCE_D_WORD, keyword, true);
        setStyles(*this, {SCE_D_WORD2, SCE_D_WORD3, SCE_D_TYPEDEF}, type);
        applyStyle(*this, SCE_D_NUMBER, number);
        setStyles(*this, {SCE_D_STRING, SCE_D_CHARACTER, SCE_D_STRINGB, SCE_D_STRINGR}, string);
        setStyles(*this, {SCE_D_STRINGEOL, SCE_D_COMMENTDOCKEYWORDERROR}, palette.danger);
    } else if (lexerName_ == QStringLiteral("julia")) {
        setStyles(*this, {SCE_JULIA_COMMENT, SCE_JULIA_DOCSTRING}, comment, false, true);
        applyStyle(*this, SCE_JULIA_KEYWORD1, keyword, true);
        setStyles(*this,
                  {SCE_JULIA_KEYWORD2, SCE_JULIA_KEYWORD3, SCE_JULIA_KEYWORD4, SCE_JULIA_TYPEANNOT},
                  type);
        applyStyle(*this, SCE_JULIA_NUMBER, number);
        setStyles(*this,
                  {SCE_JULIA_STRING, SCE_JULIA_CHAR, SCE_JULIA_STRINGLITERAL, SCE_JULIA_COMMAND,
                   SCE_JULIA_COMMANDLITERAL},
                  string);
        setStyles(*this, {SCE_JULIA_MACRO, SCE_JULIA_SYMBOL, SCE_JULIA_STRINGINTERP}, attribute);
        applyStyle(*this, SCE_JULIA_LEXERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("nim")) {
        setStyles(
            *this,
            {SCE_NIM_COMMENT, SCE_NIM_COMMENTDOC, SCE_NIM_COMMENTLINE, SCE_NIM_COMMENTLINEDOC},
            comment, false, true);
        applyStyle(*this, SCE_NIM_WORD, keyword, true);
        applyStyle(*this, SCE_NIM_NUMBER, number);
        setStyles(*this,
                  {SCE_NIM_STRING, SCE_NIM_CHARACTER, SCE_NIM_TRIPLE, SCE_NIM_TRIPLEDOUBLE,
                   SCE_NIM_BACKTICKS},
                  string);
        applyStyle(*this, SCE_NIM_FUNCNAME, type, true);
        setStyles(*this, {SCE_NIM_STRINGEOL, SCE_NIM_NUMERROR}, palette.danger);
    } else if (lexerName_ == QStringLiteral("latex")) {
        setStyles(*this, {SCE_L_COMMENT, SCE_L_COMMENT2}, comment, false, true);
        setStyles(*this, {SCE_L_COMMAND, SCE_L_SHORTCMD}, keyword, true);
        setStyles(*this, {SCE_L_TAG, SCE_L_TAG2}, type);
        setStyles(*this, {SCE_L_MATH, SCE_L_MATH2}, attribute);
        setStyles(*this, {SCE_L_VERBATIM, SCE_L_CMDOPT}, string);
        applyStyle(*this, SCE_L_ERROR, palette.danger);
    } else if (lexerName_ == QStringLiteral("asm")) {
        setStyles(*this, {SCE_ASM_COMMENT, SCE_ASM_COMMENTBLOCK, SCE_ASM_COMMENTDIRECTIVE}, comment,
                  false, true);
        setStyles(*this, {SCE_ASM_CPUINSTRUCTION, SCE_ASM_MATHINSTRUCTION, SCE_ASM_EXTINSTRUCTION},
                  keyword, true);
        applyStyle(*this, SCE_ASM_REGISTER, type);
        setStyles(*this, {SCE_ASM_DIRECTIVE, SCE_ASM_DIRECTIVEOPERAND}, attribute);
        applyStyle(*this, SCE_ASM_NUMBER, number);
        setStyles(*this, {SCE_ASM_STRING, SCE_ASM_CHARACTER, SCE_ASM_STRINGBACKQUOTE}, string);
        applyStyle(*this, SCE_ASM_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("coffeescript")) {
        setStyles(*this,
                  {SCE_COFFEESCRIPT_COMMENT, SCE_COFFEESCRIPT_COMMENTLINE,
                   SCE_COFFEESCRIPT_COMMENTDOC, SCE_COFFEESCRIPT_COMMENTBLOCK,
                   SCE_COFFEESCRIPT_COMMENTLINEDOC},
                  comment, false, true);
        setStyles(*this, {SCE_COFFEESCRIPT_WORD, SCE_COFFEESCRIPT_WORD2}, keyword, true);
        applyStyle(*this, SCE_COFFEESCRIPT_NUMBER, number);
        setStyles(*this,
                  {SCE_COFFEESCRIPT_STRING, SCE_COFFEESCRIPT_CHARACTER, SCE_COFFEESCRIPT_VERBATIM,
                   SCE_COFFEESCRIPT_REGEX, SCE_COFFEESCRIPT_STRINGRAW},
                  string);
        setStyles(*this, {SCE_COFFEESCRIPT_GLOBALCLASS, SCE_COFFEESCRIPT_INSTANCEPROPERTY},
                  attribute);
        applyStyle(*this, SCE_COFFEESCRIPT_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("verilog")) {
        setStyles(*this, {SCE_V_COMMENT, SCE_V_COMMENTLINE, SCE_V_COMMENTLINEBANG}, comment, false,
                  true);
        applyStyle(*this, SCE_V_WORD, keyword, true);
        setStyles(*this, {SCE_V_WORD2, SCE_V_WORD3, SCE_V_USER}, type);
        applyStyle(*this, SCE_V_NUMBER, number);
        applyStyle(*this, SCE_V_STRING, string);
        setStyles(*this, {SCE_V_PREPROCESSOR, SCE_V_INPUT, SCE_V_OUTPUT, SCE_V_INOUT}, attribute);
        applyStyle(*this, SCE_V_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("vhdl")) {
        setStyles(*this, {SCE_VHDL_COMMENT, SCE_VHDL_COMMENTLINEBANG, SCE_VHDL_BLOCK_COMMENT},
                  comment, false, true);
        applyStyle(*this, SCE_VHDL_KEYWORD, keyword, true);
        setStyles(*this,
                  {SCE_VHDL_STDOPERATOR, SCE_VHDL_STDFUNCTION, SCE_VHDL_STDPACKAGE,
                   SCE_VHDL_STDTYPE, SCE_VHDL_USERWORD},
                  type);
        applyStyle(*this, SCE_VHDL_ATTRIBUTE, attribute);
        applyStyle(*this, SCE_VHDL_NUMBER, number);
        applyStyle(*this, SCE_VHDL_STRING, string);
        applyStyle(*this, SCE_VHDL_STRINGEOL, palette.danger);
    } else if (lexerName_ == QStringLiteral("caml")) {
        setStyles(*this,
                  {SCE_CAML_COMMENT, SCE_CAML_COMMENT1, SCE_CAML_COMMENT2, SCE_CAML_COMMENT3},
                  comment, false, true);
        applyStyle(*this, SCE_CAML_KEYWORD, keyword, true);
        setStyles(*this, {SCE_CAML_KEYWORD2, SCE_CAML_KEYWORD3, SCE_CAML_TAGNAME}, type);
        applyStyle(*this, SCE_CAML_NUMBER, number);
        setStyles(*this, {SCE_CAML_STRING, SCE_CAML_CHAR}, string);
        applyStyle(*this, SCE_CAML_LINENUM, attribute);
    } else if (lexerName_ == QStringLiteral("fsharp")) {
        setStyles(*this, {SCE_FSHARP_COMMENT, SCE_FSHARP_COMMENTLINE}, comment, false, true);
        applyStyle(*this, SCE_FSHARP_KEYWORD, keyword, true);
        setStyles(
            *this,
            {SCE_FSHARP_KEYWORD2, SCE_FSHARP_KEYWORD3, SCE_FSHARP_KEYWORD4, SCE_FSHARP_KEYWORD5},
            type);
        applyStyle(*this, SCE_FSHARP_NUMBER, number);
        setStyles(*this,
                  {SCE_FSHARP_STRING, SCE_FSHARP_CHARACTER, SCE_FSHARP_VERBATIM,
                   SCE_FSHARP_QUOTATION, SCE_FSHARP_FORMAT_SPEC},
                  string);
        setStyles(*this, {SCE_FSHARP_PREPROCESSOR, SCE_FSHARP_ATTRIBUTE}, attribute);
    } else if (lexerName_ == QStringLiteral("gdscript")) {
        setStyles(*this, {SCE_GD_COMMENTLINE, SCE_GD_COMMENTBLOCK}, comment, false, true);
        setStyles(*this, {SCE_GD_WORD, SCE_GD_WORD2}, keyword, true);
        applyStyle(*this, SCE_GD_NUMBER, number);
        setStyles(*this, {SCE_GD_STRING, SCE_GD_CHARACTER, SCE_GD_TRIPLE, SCE_GD_TRIPLEDOUBLE},
                  string);
        setStyles(*this, {SCE_GD_CLASSNAME, SCE_GD_FUNCNAME}, type, true);
        setStyles(*this, {SCE_GD_ANNOTATION, SCE_GD_NODEPATH}, attribute);
        applyStyle(*this, SCE_GD_STRINGEOL, palette.danger);
    }
}

} // namespace ketplus

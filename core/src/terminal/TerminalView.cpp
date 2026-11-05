#include "terminal/TerminalView.h"

#include "terminal/TerminalSession.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>
#include <QToolButton>
#include <QWheelEvent>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <utility>

namespace ketplus {

TerminalView::TerminalView(TerminalSession& session, QWidget* parent)
    : QAbstractScrollArea(parent), session_(session), cursorTimer_(new QTimer(this)),
      renderTimer_(new QTimer(this)), searchTimer_(new QTimer(this)),
      searchBar_(new QWidget(viewport())), searchEdit_(new QLineEdit(searchBar_)),
      searchStatus_(new QLabel(searchBar_)) {
    setProperty("kvRole", QStringLiteral("terminalView"));
    setAccessibleName(QStringLiteral("Terminal"));
    setAccessibleDescription(QStringLiteral("Interactive embedded terminal"));
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    viewport()->setMouseTracking(true);
    viewport()->setAutoFillBackground(false);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    terminalFont.setPixelSize(13);
    setFont(terminalFont);

    searchBar_->setProperty("kvRole", QStringLiteral("terminalSearch"));
    searchBar_->setObjectName(QStringLiteral("terminalSearchBar"));
    searchBar_->setAutoFillBackground(true);
    auto* searchLayout = new QHBoxLayout(searchBar_);
    searchLayout->setContentsMargins(6, 4, 4, 4);
    searchLayout->setSpacing(4);
    searchEdit_->setPlaceholderText(QStringLiteral("Find in terminal"));
    searchEdit_->setObjectName(QStringLiteral("terminalSearchEdit"));
    searchEdit_->setAccessibleName(QStringLiteral("Find in terminal"));
    searchEdit_->installEventFilter(this);
    searchStatus_->setMinimumWidth(48);
    searchStatus_->setObjectName(QStringLiteral("terminalSearchStatus"));
    searchStatus_->setAlignment(Qt::AlignCenter);
    auto* previousButton = new QToolButton(searchBar_);
    previousButton->setText(QString::fromUtf8("↑"));
    previousButton->setToolTip(QStringLiteral("Previous match"));
    auto* nextButton = new QToolButton(searchBar_);
    nextButton->setText(QString::fromUtf8("↓"));
    nextButton->setToolTip(QStringLiteral("Next match"));
    auto* closeButton = new QToolButton(searchBar_);
    closeButton->setText(QString::fromUtf8("×"));
    closeButton->setToolTip(QStringLiteral("Close search"));
    searchLayout->addWidget(searchEdit_, 1);
    searchLayout->addWidget(searchStatus_);
    searchLayout->addWidget(previousButton);
    searchLayout->addWidget(nextButton);
    searchLayout->addWidget(closeButton);
    searchBar_->hide();

    connect(searchEdit_, &QLineEdit::textChanged, this, [this] { searchTimer_->start(); });
    connect(searchEdit_, &QLineEdit::returnPressed, this, [this] { findNext(false); });
    connect(previousButton, &QToolButton::clicked, this, [this] { findNext(true); });
    connect(nextButton, &QToolButton::clicked, this, [this] { findNext(false); });
    connect(closeButton, &QToolButton::clicked, this, &TerminalView::closeSearch);

    cursorTimer_->setInterval(530);
    connect(cursorTimer_, &QTimer::timeout, this, [this] {
        if (session_.synchronizedOutput()) {
            return;
        }
        cursorBlinkOn_ = !cursorBlinkOn_;
        viewport()->update();
    });
    cursorTimer_->start();

    renderTimer_->setInterval(16);
    renderTimer_->setTimerType(Qt::PreciseTimer);
    connect(renderTimer_, &QTimer::timeout, this, [this] {
        if (!renderPending_) {
            renderTimer_->stop();
            return;
        }
        renderPending_ = false;
        viewport()->update();
    });

    searchTimer_->setSingleShot(true);
    searchTimer_->setInterval(80);
    connect(searchTimer_, &QTimer::timeout, this, &TerminalView::rebuildSearchMatches);

    connect(&session_, &TerminalSession::contentChanged, this, [this] {
        const bool wasAtBottom = verticalScrollBar()->value() == verticalScrollBar()->maximum();
        updateScrollBar(wasAtBottom);
        cursorBlinkOn_ = true;
        if (searchBar_->isVisible()) {
            searchTimer_->start();
        }
        requestRender();
    });
    connect(&session_, &TerminalSession::historyTrimmed, this,
            &TerminalView::adjustSelectionForTrimmedHistory);
    connect(&session_, &TerminalSession::alternateScreenChanged, this, [this] {
        clearSelection();
        updateScrollBar(true);
    });
    connect(&session_, &TerminalSession::titleChanged, this, &TerminalView::titleChanged);
    connect(&session_, &TerminalSession::statusMessageRequested, this,
            &TerminalView::statusMessageRequested);
    connect(&session_, &TerminalSession::bellRequested, this, [] { QApplication::beep(); });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] { requestRender(); });

    updateGeometryFromViewport();
}

void TerminalView::applyTheme(const ThemePalette& palette) {
    foreground_ = QColor(palette.textMain);
    background_ = QColor(palette.panelSubtle);
    cursorColor_ = QColor(palette.accent);
    selectionColor_ = QColor(palette.accentMuted);
    searchColor_ = QColor(palette.warning);
    searchColor_.setAlpha(90);
    currentSearchColor_ = QColor(palette.warning);
    currentSearchColor_.setAlpha(170);
    viewport()->setAutoFillBackground(false);
    QPalette viewportPalette = viewport()->palette();
    viewportPalette.setColor(QPalette::Window, background_);
    viewportPalette.setColor(QPalette::Base, background_);
    viewport()->setPalette(viewportPalette);
    session_.setDefaultColors(foreground_.red(), foreground_.green(), foreground_.blue(),
                              background_.red(), background_.green(), background_.blue());
    viewport()->update();
}

void TerminalView::setTypography(const int fontSizePixels, const int lineHeightPixels) {
    QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    terminalFont.setPixelSize(qBound(8, fontSizePixels, 48));
    setFont(terminalFont);
    lineHeightPixels_ = qBound(qMax(12, terminalFont.pixelSize()), lineHeightPixels, 96);
    updateGeometryFromViewport();
    viewport()->update();
}

void TerminalView::start(const QString& workingDirectory) {
    updateGeometryFromViewport();
    if (session_.start(workingDirectory)) {
        setFocus();
    }
}

bool TerminalView::restart() {
    clearSelection();
    const bool started = session_.restart();
    if (started) {
        setFocus();
    }
    return started;
}

void TerminalView::pasteClipboard() { session_.sendPaste(QApplication::clipboard()->text()); }

void TerminalView::copySelection() {
    const QString text = selectedText();
    if (text.isEmpty()) {
        return;
    }
    QClipboard* clipboard = QApplication::clipboard();
    clipboard->setText(text, QClipboard::Clipboard);
    if (clipboard->supportsSelection()) {
        clipboard->setText(text, QClipboard::Selection);
    }
}

void TerminalView::selectAll() {
    if (session_.contentLineCount() <= 0 || session_.columns() <= 0) {
        return;
    }
    selectionAnchor_ = CellPosition{0, 0};
    selectionHead_ = CellPosition{session_.contentLineCount() - 1, session_.columns() - 1};
    selectionActive_ = true;
    viewport()->update();
}

void TerminalView::clearSelection() {
    if (!selectionActive_) {
        return;
    }
    selectionActive_ = false;
    selecting_ = false;
    viewport()->update();
}

void TerminalView::clearScrollback() {
    clearSelection();
    session_.clearScrollback();
}

void TerminalView::openSearch() {
    searchBar_->show();
    searchBar_->raise();
    updateSearchGeometry();
    searchEdit_->setFocus(Qt::ShortcutFocusReason);
    searchEdit_->selectAll();
    rebuildSearchMatches();
}

QSize TerminalView::sizeHint() const { return QSize(720, 220); }

QString TerminalView::visibleText() const {
    QStringList lines;
    lines.reserve(session_.rows());
    for (int row = 0; row < session_.rows(); ++row) {
        lines.append(session_.lineText(historyOffset() + row));
    }
    return lines.join(QLatin1Char('\n'));
}

QString TerminalView::selectedText() const {
    if (!selectionActive_) {
        return {};
    }
    CellPosition first = selectionAnchor_;
    CellPosition last = selectionHead_;
    if (positionLess(last, first)) {
        std::swap(first, last);
    }

    QStringList lines;
    for (int row = first.row; row <= last.row; ++row) {
        const int firstColumn = row == first.row ? first.column : 0;
        const int lastColumn = row == last.row ? last.column : session_.columns() - 1;
        QString line;
        for (int column = firstColumn; column <= lastColumn; ++column) {
            const QString cellText = session_.textForCell(session_.cellAt(row, column));
            line.append(cellText.isEmpty() ? QStringLiteral(" ") : cellText);
        }
        while (line.endsWith(QLatin1Char(' '))) {
            line.chop(1);
        }
        lines.append(line);
    }
    return lines.join(QLatin1Char('\n'));
}

bool TerminalView::hasSelection() const noexcept { return selectionActive_; }

void TerminalView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), background_);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const int offset = historyOffset();
    const QFont baseFont = font();
    const QFontMetrics baseMetrics(baseFont);
    const int baseline = baseMetrics.ascent() + qMax(0, (cellHeight_ - baseMetrics.height()) / 2);
    for (int row = 0; row < session_.rows(); ++row) {
        const int y = topPadding_ + row * cellHeight_;
        if (y >= viewport()->height()) {
            break;
        }
        const int absoluteRow = offset + row;
        for (int column = 0; column < session_.columns(); ++column) {
            VTermScreenCell cell = cellAtVisiblePosition(row, column);
            QColor foreground = colorFor(cell.fg, true);
            QColor background = colorFor(cell.bg, false);
            if (cell.attrs.reverse != 0U) {
                std::swap(foreground, background);
            }
            const QRect cellRect(leftPadding_ + column * cellWidth_, y, cellWidth_, cellHeight_);
            if (background != background_) {
                painter.fillRect(cellRect, background);
            }
            const int matchIndex = searchMatchAt(absoluteRow, column);
            if (matchIndex >= 0) {
                painter.fillRect(cellRect, matchIndex == currentSearchMatch_ ? currentSearchColor_
                                                                             : searchColor_);
            }
            if (cellIsSelected(absoluteRow, column)) {
                painter.fillRect(cellRect, selectionColor_);
            }

            const QString text = session_.textForCell(cell);
            if (!text.isEmpty() && cell.attrs.conceal == 0U) {
                QFont drawFont = baseFont;
                drawFont.setBold(cell.attrs.bold != 0U);
                drawFont.setItalic(cell.attrs.italic != 0U);
                drawFont.setUnderline(cell.attrs.underline != 0U);
                drawFont.setStrikeOut(cell.attrs.strike != 0U);
                painter.setFont(drawFont);
                painter.setPen(foreground);
                painter.drawText(cellRect.left(), cellRect.top() + baseline, text);
            }
        }
    }

    const VTermPos cursor = session_.cursorPosition();
    const int cursorDisplayRow =
        (session_.alternateScreen() ? 0 : session_.historyLineCount()) + cursor.row - offset;
    int preeditCursorOffset = 0;
    if (!preeditText_.isEmpty() && cursorDisplayRow >= 0 && cursorDisplayRow < session_.rows()) {
        const int preeditX = leftPadding_ + cursor.col * cellWidth_;
        const int preeditY = topPadding_ + cursorDisplayRow * cellHeight_;
        painter.setFont(baseFont);
        painter.setPen(foreground_);
        painter.drawText(preeditX, preeditY + baseline, preeditText_);
        preeditCursorOffset =
            QFontMetrics(baseFont).horizontalAdvance(preeditText_.left(preeditCursor_));
    }
    if (session_.cursorVisible() && cursorBlinkOn_ && hasFocus() && cursorDisplayRow >= 0 &&
        cursorDisplayRow < session_.rows()) {
        const QRect cursorRect(leftPadding_ + cursor.col * cellWidth_ + preeditCursorOffset,
                               topPadding_ + cursorDisplayRow * cellHeight_, cellWidth_,
                               cellHeight_);
        QColor translucentCursor = cursorColor_;
        translucentCursor.setAlpha(150);
        painter.fillRect(cursorRect, translucentCursor);
    }
}

void TerminalView::resizeEvent(QResizeEvent* event) {
    QAbstractScrollArea::resizeEvent(event);
    updateGeometryFromViewport();
    updateSearchGeometry();
}

bool TerminalView::event(QEvent* event) {
    if (event->type() == QEvent::ShortcutOverride) {
        const auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape && keyEvent->modifiers() == Qt::NoModifier) {
            event->accept();
            return true;
        }
#if defined(Q_OS_MACOS)
        if (keyEvent->matches(QKeySequence::Copy) || keyEvent->matches(QKeySequence::Paste) ||
            keyEvent->matches(QKeySequence::Find) || keyEvent->matches(QKeySequence::SelectAll)) {
            event->accept();
            return true;
        }
#endif
    }
    return QAbstractScrollArea::event(event);
}

bool TerminalView::eventFilter(QObject* watched, QEvent* event) {
    if (watched == searchEdit_ && event->type() == QEvent::KeyPress) {
        auto* keyEvent = static_cast<QKeyEvent*>(event);
        if (keyEvent->key() == Qt::Key_Escape) {
            closeSearch();
            return true;
        }
        if ((keyEvent->key() == Qt::Key_Return || keyEvent->key() == Qt::Key_Enter) &&
            (keyEvent->modifiers() & Qt::ShiftModifier) != 0) {
            findNext(true);
            return true;
        }
    }
    return QAbstractScrollArea::eventFilter(watched, event);
}

void TerminalView::keyPressEvent(QKeyEvent* event) {
#if defined(Q_OS_MACOS)
    if (event->matches(QKeySequence::Paste)) {
        pasteClipboard();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Copy)) {
        copySelection();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Find)) {
        openSearch();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::SelectAll)) {
        selectAll();
        event->accept();
        return;
    }
#else
    const auto keyModifiers = event->modifiers();
    const bool terminalClipboardShortcut =
        (keyModifiers & (Qt::ControlModifier | Qt::ShiftModifier)) ==
        (Qt::ControlModifier | Qt::ShiftModifier);
    if (terminalClipboardShortcut && event->key() == Qt::Key_V) {
        pasteClipboard();
        event->accept();
        return;
    }
    if (terminalClipboardShortcut && event->key() == Qt::Key_C) {
        copySelection();
        event->accept();
        return;
    }
    if (terminalClipboardShortcut && event->key() == Qt::Key_F) {
        openSearch();
        event->accept();
        return;
    }
    if (terminalClipboardShortcut && event->key() == Qt::Key_A) {
        selectAll();
        event->accept();
        return;
    }
#endif
    if (!session_.isRunning()) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
#if defined(Q_OS_MACOS)
    if ((event->modifiers() & Qt::ControlModifier) != 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
#else
    if ((event->modifiers() & Qt::MetaModifier) != 0) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
#endif

    clearSelection();
    VTermKey key = VTERM_KEY_NONE;
    switch (event->key()) {
    case Qt::Key_Return:
    case Qt::Key_Enter:
        key = VTERM_KEY_ENTER;
        break;
    case Qt::Key_Backspace:
        key = VTERM_KEY_BACKSPACE;
        break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab:
        key = VTERM_KEY_TAB;
        break;
    case Qt::Key_Escape:
        key = VTERM_KEY_ESCAPE;
        break;
    case Qt::Key_Up:
        key = VTERM_KEY_UP;
        break;
    case Qt::Key_Down:
        key = VTERM_KEY_DOWN;
        break;
    case Qt::Key_Left:
        key = VTERM_KEY_LEFT;
        break;
    case Qt::Key_Right:
        key = VTERM_KEY_RIGHT;
        break;
    case Qt::Key_Insert:
        key = VTERM_KEY_INS;
        break;
    case Qt::Key_Delete:
        key = VTERM_KEY_DEL;
        break;
    case Qt::Key_Home:
        key = VTERM_KEY_HOME;
        break;
    case Qt::Key_End:
        key = VTERM_KEY_END;
        break;
    case Qt::Key_PageUp:
        key = VTERM_KEY_PAGEUP;
        break;
    case Qt::Key_PageDown:
        key = VTERM_KEY_PAGEDOWN;
        break;
    default:
        if (event->key() >= Qt::Key_F1 && event->key() <= Qt::Key_F35) {
            key = static_cast<VTermKey>(VTERM_KEY_FUNCTION(event->key() - Qt::Key_F1 + 1));
        }
        break;
    }

    if (key != VTERM_KEY_NONE) {
        session_.sendKey(key, modifiersFor(event));
        event->accept();
        return;
    }

    const VTermModifier terminalModifiers = modifiersFor(event);
    if ((terminalModifiers & VTERM_MOD_CTRL) != 0 && event->key() >= Qt::Key_A &&
        event->key() <= Qt::Key_Z) {
        const uint character =
            static_cast<uint>('a' + (event->key() - static_cast<int>(Qt::Key_A)));
        session_.sendCodepoint(
            character,
            static_cast<VTermModifier>(terminalModifiers & (VTERM_MOD_CTRL | VTERM_MOD_ALT)));
        event->accept();
        return;
    }

    const QList<uint> characters = event->text().toUcs4();
    if (!characters.isEmpty()) {
        const VTermModifier modifiers =
            static_cast<VTermModifier>(terminalModifiers & (VTERM_MOD_CTRL | VTERM_MOD_ALT));
        for (const uint character : characters) {
            session_.sendCodepoint(character, modifiers);
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void TerminalView::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    const bool selectionOverride = (event->modifiers() & Qt::ShiftModifier) != 0;
    if (session_.mouseTrackingEnabled() && !selectionOverride) {
        const CellPosition position = viewportPositionForPoint(event->position().toPoint());
        int button = 0;
        if (event->button() == Qt::LeftButton) {
            button = 1;
        } else if (event->button() == Qt::MiddleButton) {
            button = 2;
        } else if (event->button() == Qt::RightButton) {
            button = 3;
        }
        session_.sendMouseMove(position.row, position.column, modifiersFor(event->modifiers()));
        if (button != 0) {
            session_.sendMouseButton(button, true, modifiersFor(event->modifiers()));
        }
        event->accept();
        return;
    }
    if (event->button() == Qt::LeftButton) {
        selectionAnchor_ = positionForPoint(event->position().toPoint());
        selectionHead_ = selectionAnchor_;
        selectionActive_ = true;
        selecting_ = true;
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void TerminalView::mouseMoveEvent(QMouseEvent* event) {
    const bool selectionOverride = (event->modifiers() & Qt::ShiftModifier) != 0;
    if (session_.mouseTrackingEnabled() && !selectionOverride) {
        const CellPosition position = viewportPositionForPoint(event->position().toPoint());
        session_.sendMouseMove(position.row, position.column, modifiersFor(event->modifiers()));
        event->accept();
        return;
    }
    if (selecting_ && (event->buttons() & Qt::LeftButton) != 0) {
        selectionHead_ = positionForPoint(event->position().toPoint());
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void TerminalView::mouseReleaseEvent(QMouseEvent* event) {
    const bool selectionOverride = (event->modifiers() & Qt::ShiftModifier) != 0;
    if (session_.mouseTrackingEnabled() && !selectionOverride) {
        const CellPosition position = viewportPositionForPoint(event->position().toPoint());
        int button = 0;
        if (event->button() == Qt::LeftButton) {
            button = 1;
        } else if (event->button() == Qt::MiddleButton) {
            button = 2;
        } else if (event->button() == Qt::RightButton) {
            button = 3;
        }
        session_.sendMouseMove(position.row, position.column, modifiersFor(event->modifiers()));
        if (button != 0) {
            session_.sendMouseButton(button, false, modifiersFor(event->modifiers()));
        }
        event->accept();
        return;
    }
    if (selecting_ && event->button() == Qt::LeftButton) {
        selectionHead_ = positionForPoint(event->position().toPoint());
        selecting_ = false;
        viewport()->update();
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

void TerminalView::mouseDoubleClickEvent(QMouseEvent* event) {
    if (session_.mouseTrackingEnabled() && (event->modifiers() & Qt::ShiftModifier) == 0) {
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) {
        QAbstractScrollArea::mouseDoubleClickEvent(event);
        return;
    }
    const CellPosition position = positionForPoint(event->position().toPoint());
    const QString line = session_.lineText(position.row, false);
    int first = std::clamp(position.column, 0, std::max(0, static_cast<int>(line.size()) - 1));
    int last = first;
    const auto isWord = [](const QChar character) {
        return character.isLetterOrNumber() || character == QLatin1Char('_') ||
               character == QLatin1Char('-') || character == QLatin1Char('.') ||
               character == QLatin1Char('/') || character == QLatin1Char('~');
    };
    const bool word = !line.isEmpty() && isWord(line.at(first));
    while (first > 0 && (isWord(line.at(first - 1)) == word) && !line.at(first - 1).isSpace()) {
        --first;
    }
    while (last + 1 < line.size() && (isWord(line.at(last + 1)) == word) &&
           !line.at(last + 1).isSpace()) {
        ++last;
    }
    selectionAnchor_ = CellPosition{position.row, first};
    selectionHead_ = CellPosition{position.row, last};
    selectionActive_ = true;
    selecting_ = false;
    viewport()->update();
    event->accept();
}

void TerminalView::wheelEvent(QWheelEvent* event) {
    if (session_.mouseTrackingEnabled() && (event->modifiers() & Qt::ShiftModifier) == 0) {
        const int delta =
            event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
        if (delta != 0) {
            const CellPosition position = viewportPositionForPoint(event->position().toPoint());
            const VTermModifier modifiers = modifiersFor(event->modifiers());
            session_.sendMouseMove(position.row, position.column, modifiers);
            const int button = delta > 0 ? 4 : 5;
            const int clicks = std::clamp(std::abs(delta) / 120, 1, 10);
            for (int click = 0; click < clicks; ++click) {
                session_.sendMouseButton(button, true, modifiers);
                session_.sendMouseButton(button, false, modifiers);
            }
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void TerminalView::contextMenuEvent(QContextMenuEvent* event) {
    if (session_.mouseTrackingEnabled() && (event->modifiers() & Qt::ShiftModifier) == 0) {
        event->accept();
        return;
    }
    QMenu menu(this);
    QAction* copyAction =
        menu.addAction(QStringLiteral("Copy"), this, &TerminalView::copySelection);
    copyAction->setEnabled(hasSelection());
    QAction* pasteAction =
        menu.addAction(QStringLiteral("Paste"), this, &TerminalView::pasteClipboard);
    pasteAction->setEnabled(session_.isRunning() && !QApplication::clipboard()->text().isEmpty());
    menu.addAction(QStringLiteral("Select All"), this, &TerminalView::selectAll);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Find…"), this, &TerminalView::openSearch);
    menu.addAction(QStringLiteral("Clear Scrollback"), this, &TerminalView::clearScrollback);
    menu.addSeparator();
    menu.addAction(QStringLiteral("Restart Terminal"), this, [this] { restart(); });
    menu.exec(event->globalPos());
}

void TerminalView::inputMethodEvent(QInputMethodEvent* event) {
    preeditText_ = event->preeditString();
    preeditCursor_ = preeditText_.size();
    for (const QInputMethodEvent::Attribute& attribute : event->attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            preeditCursor_ = std::clamp(attribute.start, 0, static_cast<int>(preeditText_.size()));
            break;
        }
    }
    for (const uint character : event->commitString().toUcs4()) {
        session_.sendCodepoint(character, VTERM_MOD_NONE);
    }
    requestRender();
    event->accept();
}

QVariant TerminalView::inputMethodQuery(const Qt::InputMethodQuery query) const {
    const VTermPos cursor = session_.cursorPosition();
    if (query == Qt::ImCursorRectangle) {
        return QRect(leftPadding_ + cursor.col * cellWidth_, topPadding_ + cursor.row * cellHeight_,
                     cellWidth_, cellHeight_);
    }
    if (query == Qt::ImEnabled) {
        return true;
    }
    if (query == Qt::ImFont) {
        return font();
    }
    if (query == Qt::ImCursorPosition || query == Qt::ImAnchorPosition) {
        return preeditCursor_;
    }
    return QAbstractScrollArea::inputMethodQuery(query);
}

void TerminalView::focusInEvent(QFocusEvent* event) {
    QAbstractScrollArea::focusInEvent(event);
    cursorBlinkOn_ = true;
    session_.setFocused(true);
    requestRender();
}

void TerminalView::focusOutEvent(QFocusEvent* event) {
    QAbstractScrollArea::focusOutEvent(event);
    session_.setFocused(false);
    requestRender();
}

void TerminalView::requestRender() {
    renderPending_ = true;
    if (!renderTimer_->isActive()) {
        renderPending_ = false;
        viewport()->update();
        renderTimer_->start();
    }
}

void TerminalView::updateGeometryFromViewport() {
    const QFontMetrics metrics(font());
    cellWidth_ = std::max(1, metrics.horizontalAdvance(QLatin1Char('M')));
    cellHeight_ = std::max(metrics.height(), lineHeightPixels_);
    const int availableWidth = std::max(1, viewport()->width() - leftPadding_ * 2);
    const int availableHeight = std::max(1, viewport()->height() - topPadding_ * 2);
    session_.resizeTerminal(std::max(2, availableHeight / cellHeight_),
                            std::max(2, availableWidth / cellWidth_));
    updateScrollBar(true);
}

void TerminalView::updateScrollBar(const bool preserveBottom) {
    QScrollBar* bar = verticalScrollBar();
    const int maximum = session_.alternateScreen() ? 0 : session_.historyLineCount();
    bar->setRange(0, maximum);
    bar->setPageStep(session_.rows());
    if (preserveBottom) {
        bar->setValue(maximum);
    }
}

void TerminalView::updateSearchGeometry() {
    if (!searchBar_->isVisible()) {
        return;
    }
    const int width = std::min(430, std::max(260, viewport()->width() - 24));
    searchBar_->adjustSize();
    searchBar_->resize(width, searchBar_->sizeHint().height());
    searchBar_->move(std::max(6, viewport()->width() - width - 10), 8);
}

void TerminalView::rebuildSearchMatches() {
    std::optional<SearchMatch> previousMatch;
    if (currentSearchMatch_ >= 0 && currentSearchMatch_ < searchMatches_.size()) {
        previousMatch = searchMatches_.at(currentSearchMatch_);
    }
    searchMatches_.clear();
    currentSearchMatch_ = -1;
    const QString query = searchEdit_->text();
    if (query.isEmpty()) {
        searchStatus_->clear();
        viewport()->update();
        return;
    }

    for (int row = 0; row < session_.contentLineCount(); ++row) {
        QString line;
        QVector<int> columnsForCharacter;
        for (int column = 0; column < session_.columns(); ++column) {
            QString cellText = session_.textForCell(session_.cellAt(row, column));
            if (cellText.isEmpty()) {
                cellText = QStringLiteral(" ");
            }
            line.append(cellText);
            for (int character = 0; character < cellText.size(); ++character) {
                columnsForCharacter.append(column);
            }
        }
        qsizetype from = 0;
        while ((from = line.indexOf(query, from, Qt::CaseInsensitive)) >= 0) {
            const qsizetype end = from + query.size() - 1;
            if (from < columnsForCharacter.size() && end < columnsForCharacter.size()) {
                searchMatches_.append(
                    SearchMatch{row, columnsForCharacter.at(from), columnsForCharacter.at(end)});
            }
            from += std::max<qsizetype>(1, query.size());
        }
    }

    if (searchMatches_.isEmpty()) {
        searchStatus_->setText(QStringLiteral("No matches"));
    } else {
        int nextIndex = 0;
        if (previousMatch.has_value()) {
            for (int index = 0; index < searchMatches_.size(); ++index) {
                const SearchMatch& match = searchMatches_.at(index);
                if (match.row == previousMatch->row &&
                    match.firstColumn == previousMatch->firstColumn &&
                    match.lastColumn == previousMatch->lastColumn) {
                    nextIndex = index;
                    break;
                }
            }
        }
        activateSearchMatch(nextIndex);
    }
    viewport()->update();
}

void TerminalView::activateSearchMatch(const int index) {
    if (searchMatches_.isEmpty()) {
        currentSearchMatch_ = -1;
        return;
    }
    currentSearchMatch_ =
        (index % searchMatches_.size() + searchMatches_.size()) % searchMatches_.size();
    const SearchMatch& match = searchMatches_.at(currentSearchMatch_);
    if (!session_.alternateScreen()) {
        verticalScrollBar()->setValue(std::clamp(match.row, 0, verticalScrollBar()->maximum()));
    }
    searchStatus_->setText(
        QStringLiteral("%1 / %2").arg(currentSearchMatch_ + 1).arg(searchMatches_.size()));
    viewport()->update();
}

void TerminalView::findNext(const bool backwards) {
    if (searchMatches_.isEmpty()) {
        rebuildSearchMatches();
        return;
    }
    activateSearchMatch(currentSearchMatch_ + (backwards ? -1 : 1));
}

void TerminalView::closeSearch() {
    searchTimer_->stop();
    searchBar_->hide();
    searchMatches_.clear();
    currentSearchMatch_ = -1;
    setFocus(Qt::ShortcutFocusReason);
    viewport()->update();
}

void TerminalView::adjustSelectionForTrimmedHistory(const int lineCount) {
    if (!selectionActive_ || lineCount <= 0) {
        return;
    }
    if (selectionAnchor_.row < lineCount || selectionHead_.row < lineCount) {
        clearSelection();
        return;
    }
    selectionAnchor_.row -= lineCount;
    selectionHead_.row -= lineCount;
}

VTermModifier TerminalView::modifiersFor(QKeyEvent* event) const {
    return modifiersFor(event->modifiers());
}

VTermModifier TerminalView::modifiersFor(const Qt::KeyboardModifiers keyboardModifiers) const {
    int modifiers = VTERM_MOD_NONE;
    if ((keyboardModifiers & Qt::ShiftModifier) != 0) {
        modifiers |= VTERM_MOD_SHIFT;
    }
    if ((keyboardModifiers & Qt::AltModifier) != 0) {
        modifiers |= VTERM_MOD_ALT;
    }
#if defined(Q_OS_MACOS)
    if ((keyboardModifiers & Qt::MetaModifier) != 0) {
#else
    if ((keyboardModifiers & Qt::ControlModifier) != 0) {
#endif
        modifiers |= VTERM_MOD_CTRL;
    }
    return static_cast<VTermModifier>(modifiers);
}

QColor TerminalView::colorFor(VTermColor color, const bool foreground) const {
    if ((foreground && VTERM_COLOR_IS_DEFAULT_FG(&color)) ||
        (!foreground && VTERM_COLOR_IS_DEFAULT_BG(&color))) {
        return foreground ? foreground_ : background_;
    }
    session_.convertColorToRgb(color);
    return QColor(color.rgb.red, color.rgb.green, color.rgb.blue);
}

VTermScreenCell TerminalView::cellAtVisiblePosition(const int displayRow, const int column) const {
    return session_.cellAt(historyOffset() + displayRow, column);
}

int TerminalView::historyOffset() const {
    return session_.alternateScreen() ? 0 : verticalScrollBar()->value();
}

TerminalView::CellPosition TerminalView::positionForPoint(const QPoint& point) const {
    CellPosition position = viewportPositionForPoint(point);
    position.row += historyOffset();
    return position;
}

TerminalView::CellPosition TerminalView::viewportPositionForPoint(const QPoint& point) const {
    const int displayRow =
        std::clamp((point.y() - topPadding_) / cellHeight_, 0, std::max(0, session_.rows() - 1));
    const int column =
        std::clamp((point.x() - leftPadding_) / cellWidth_, 0, std::max(0, session_.columns() - 1));
    return CellPosition{displayRow, column};
}

bool TerminalView::cellIsSelected(const int absoluteRow, const int column) const {
    if (!selectionActive_) {
        return false;
    }
    CellPosition first = selectionAnchor_;
    CellPosition last = selectionHead_;
    if (positionLess(last, first)) {
        std::swap(first, last);
    }
    const CellPosition cell{absoluteRow, column};
    return !positionLess(cell, first) && !positionLess(last, cell);
}

int TerminalView::searchMatchAt(const int absoluteRow, const int column) const {
    auto match = std::lower_bound(
        searchMatches_.cbegin(), searchMatches_.cend(), absoluteRow,
        [](const SearchMatch& candidate, const int row) { return candidate.row < row; });
    for (; match != searchMatches_.cend() && match->row == absoluteRow; ++match) {
        if (column >= match->firstColumn && column <= match->lastColumn) {
            return static_cast<int>(std::distance(searchMatches_.cbegin(), match));
        }
    }
    return -1;
}

bool TerminalView::positionLess(const CellPosition left, const CellPosition right) noexcept {
    return left.row < right.row || (left.row == right.row && left.column < right.column);
}

} // namespace ketplus

#include "terminal/TerminalView.h"

#include "terminal/PtyProcess.h"

#include <QApplication>
#include <QClipboard>
#include <QFocusEvent>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QInputMethodEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTimer>

#include <algorithm>
#include <cstring>

namespace ketplus {
namespace {

constexpr int maximumScrollbackLines = 5000;

} // namespace

TerminalView::TerminalView(PtyProcess& process, QWidget* parent)
    : QAbstractScrollArea(parent), process_(process), terminal_(vterm_new(24, 80)),
      screen_(vterm_obtain_screen(terminal_)), cursorTimer_(new QTimer(this)),
      renderTimer_(new QTimer(this)), synchronizedOutputTimer_(new QTimer(this)) {
    static const VTermScreenCallbacks callbacks = {
        &TerminalView::damageCallback,
        nullptr,
        &TerminalView::cursorCallback,
        &TerminalView::propertyCallback,
        &TerminalView::bellCallback,
        &TerminalView::resizeCallback,
        &TerminalView::scrollbackPushCallback,
        &TerminalView::scrollbackPopCallback,
        &TerminalView::scrollbackClearCallback,
    };
    setProperty("kvRole", QStringLiteral("terminalView"));
    setAccessibleName(QStringLiteral("Terminal"));
    setAccessibleDescription(QStringLiteral("Interactive embedded terminal"));
    setFocusPolicy(Qt::StrongFocus);
    setAttribute(Qt::WA_InputMethodEnabled, true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent, true);
    viewport()->setAutoFillBackground(false);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

    vterm_set_utf8(terminal_, 1);
    vterm_output_set_callback(terminal_, &TerminalView::outputCallback, this);
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_ROW);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_enable_reflow(screen_, true);
    vterm_screen_reset(screen_, 1);

    cursorTimer_->setInterval(530);
    connect(cursorTimer_, &QTimer::timeout, this, [this] {
        if (synchronizedOutput_) {
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

    synchronizedOutputTimer_->setSingleShot(true);
    synchronizedOutputTimer_->setInterval(120);
    connect(synchronizedOutputTimer_, &QTimer::timeout, this, [this] {
        synchronizedOutput_ = false;
        synchronizedOutputScan_.clear();
        requestRender();
    });

    connect(&process_, &PtyProcess::outputReceived, this, &TerminalView::consumeOutput);
    connect(&process_, &PtyProcess::errorOccurred, this,
            [this](const QString& message) { emit statusMessageRequested(message); });
    connect(&process_, &PtyProcess::exited, this, [this](const int exitCode) {
        emit statusMessageRequested(
            QStringLiteral("Terminal process exited with code %1").arg(exitCode));
        viewport()->update();
    });
    connect(verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (!synchronizedOutput_) {
            requestRender();
        }
    });

    updateGeometryFromViewport();
}

TerminalView::~TerminalView() {
    if (terminal_ != nullptr) {
        vterm_free(terminal_);
    }
}

void TerminalView::applyTheme(const ThemePalette& palette) {
    foreground_ = QColor(palette.textMain);
    background_ = QColor(palette.panelSubtle);
    cursorColor_ = QColor(palette.accent);
    viewport()->setAutoFillBackground(false);
    QPalette viewportPalette = viewport()->palette();
    viewportPalette.setColor(QPalette::Window, background_);
    viewportPalette.setColor(QPalette::Base, background_);
    viewport()->setPalette(viewportPalette);

    VTermColor foreground;
    VTermColor background;
    vterm_color_rgb(&foreground, static_cast<uint8_t>(foreground_.red()),
                    static_cast<uint8_t>(foreground_.green()),
                    static_cast<uint8_t>(foreground_.blue()));
    vterm_color_rgb(&background, static_cast<uint8_t>(background_.red()),
                    static_cast<uint8_t>(background_.green()),
                    static_cast<uint8_t>(background_.blue()));
    vterm_screen_set_default_colors(screen_, &foreground, &background);
    viewport()->update();
}

void TerminalView::start(const QString& workingDirectory) {
    updateGeometryFromViewport();
    if (!process_.start(workingDirectory, rows_, columns_)) {
        return;
    }
    setFocus();
}

void TerminalView::pasteClipboard() {
    if (!process_.isRunning()) {
        return;
    }
    const QString text = QApplication::clipboard()->text();
    if (text.isEmpty()) {
        return;
    }
    vterm_keyboard_start_paste(terminal_);
    process_.send(text.toUtf8());
    vterm_keyboard_end_paste(terminal_);
}

QSize TerminalView::sizeHint() const { return QSize(720, 220); }

QString TerminalView::visibleText() const {
    QStringList lines;
    lines.reserve(rows_);
    for (int row = 0; row < rows_; ++row) {
        QString line;
        for (int column = 0; column < columns_; ++column) {
            const VTermScreenCell cell = cellAtVisiblePosition(row, column);
            line.append(textForCell(cell));
        }
        while (line.endsWith(QLatin1Char(' '))) {
            line.chop(1);
        }
        lines.append(line);
    }
    return lines.join(QLatin1Char('\n'));
}

void TerminalView::paintEvent(QPaintEvent* event) {
    Q_UNUSED(event)
    QPainter painter(viewport());
    painter.fillRect(viewport()->rect(), background_);
    painter.setRenderHint(QPainter::TextAntialiasing, true);

    const int offset = historyOffset();
    const QFont baseFont = font();
    const int baseline = QFontMetrics(baseFont).ascent();
    for (int row = 0; row < rows_; ++row) {
        const int y = topPadding_ + row * cellHeight_;
        if (y >= viewport()->height()) {
            break;
        }
        for (int column = 0; column < columns_; ++column) {
            VTermScreenCell cell = cellAtVisiblePosition(row, column);
            QColor foreground = colorFor(cell.fg, true);
            QColor background = colorFor(cell.bg, false);
            if (cell.attrs.reverse != 0U) {
                std::swap(foreground, background);
            }
            const QRect cellRect(leftPadding_ + column * cellWidth_, y, cellWidth_,
                                 cellHeight_);
            if (background != background_) {
                painter.fillRect(cellRect, background);
            }

            const QString text = textForCell(cell);
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

    const int cursorDisplayRow = static_cast<int>(history_.size()) + cursor_.row - offset;
    int preeditCursorOffset = 0;
    if (!preeditText_.isEmpty() && cursorDisplayRow >= 0 && cursorDisplayRow < rows_) {
        const int preeditX = leftPadding_ + cursor_.col * cellWidth_;
        const int preeditY = topPadding_ + cursorDisplayRow * cellHeight_;
        painter.setFont(baseFont);
        painter.setPen(foreground_);
        painter.drawText(preeditX, preeditY + baseline, preeditText_);
        preeditCursorOffset =
            QFontMetrics(baseFont).horizontalAdvance(preeditText_.left(preeditCursor_));
    }
    if (cursorVisible_ && cursorBlinkOn_ && hasFocus() && cursorDisplayRow >= 0 &&
        cursorDisplayRow < rows_) {
        const QRect cursorRect(leftPadding_ + cursor_.col * cellWidth_ + preeditCursorOffset,
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
}

void TerminalView::keyPressEvent(QKeyEvent* event) {
#if defined(Q_OS_MACOS)
    if (event->matches(QKeySequence::Paste)) {
        pasteClipboard();
        event->accept();
        return;
    }
    if (event->matches(QKeySequence::Copy)) {
        event->accept();
        return;
    }
#else
    const auto modifiers = event->modifiers();
    const bool terminalClipboardShortcut =
        (modifiers & (Qt::ControlModifier | Qt::ShiftModifier)) ==
        (Qt::ControlModifier | Qt::ShiftModifier);
    if (terminalClipboardShortcut && event->key() == Qt::Key_V) {
        pasteClipboard();
        event->accept();
        return;
    }
    if (terminalClipboardShortcut && event->key() == Qt::Key_C) {
        event->accept();
        return;
    }
#endif
    if (!process_.isRunning()) {
        QAbstractScrollArea::keyPressEvent(event);
        return;
    }
#if defined(Q_OS_MACOS)
    // Qt maps the physical Command key to ControlModifier and the physical
    // Control key to MetaModifier on macOS. Command combinations belong to
    // application shortcuts; physical Control combinations belong to the PTY.
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
        sendKey(key, modifiersFor(event));
        event->accept();
        return;
    }

    const VTermModifier terminalModifiers = modifiersFor(event);
    if ((terminalModifiers & VTERM_MOD_CTRL) != 0 && event->key() >= Qt::Key_A &&
        event->key() <= Qt::Key_Z) {
        const uint character =
            static_cast<uint>('a' + (event->key() - static_cast<int>(Qt::Key_A)));
        vterm_keyboard_unichar(
            terminal_, character,
            static_cast<VTermModifier>(terminalModifiers & (VTERM_MOD_CTRL | VTERM_MOD_ALT)));
        event->accept();
        return;
    }

    const QList<uint> characters = event->text().toUcs4();
    if (!characters.isEmpty()) {
        const VTermModifier modifiers = static_cast<VTermModifier>(
            terminalModifiers & (VTERM_MOD_CTRL | VTERM_MOD_ALT));
        for (const uint character : characters) {
            vterm_keyboard_unichar(terminal_, character, modifiers);
        }
        event->accept();
        return;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void TerminalView::mousePressEvent(QMouseEvent* event) {
    setFocus(Qt::MouseFocusReason);
    requestRender();
    event->accept();
}

void TerminalView::inputMethodEvent(QInputMethodEvent* event) {
    preeditText_ = event->preeditString();
    preeditCursor_ = preeditText_.size();
    for (const QInputMethodEvent::Attribute& attribute : event->attributes()) {
        if (attribute.type == QInputMethodEvent::Cursor) {
            preeditCursor_ =
                std::clamp(attribute.start, 0, static_cast<int>(preeditText_.size()));
            break;
        }
    }
    const QList<uint> characters = event->commitString().toUcs4();
    for (const uint character : characters) {
        vterm_keyboard_unichar(terminal_, character, VTERM_MOD_NONE);
    }
    requestRender();
    event->accept();
}

QVariant TerminalView::inputMethodQuery(const Qt::InputMethodQuery query) const {
    if (query == Qt::ImCursorRectangle) {
        return QRect(leftPadding_ + cursor_.col * cellWidth_,
                     topPadding_ + cursor_.row * cellHeight_, cellWidth_, cellHeight_);
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
    vterm_state_focus_in(vterm_obtain_state(terminal_));
    requestRender();
}

void TerminalView::focusOutEvent(QFocusEvent* event) {
    QAbstractScrollArea::focusOutEvent(event);
    vterm_state_focus_out(vterm_obtain_state(terminal_));
    requestRender();
}

void TerminalView::outputCallback(const char* bytes, const size_t length, void* user) {
    auto* view = static_cast<TerminalView*>(user);
    view->process_.send(QByteArray(bytes, static_cast<qsizetype>(length)));
}

int TerminalView::damageCallback(VTermRect, void* user) {
    Q_UNUSED(user)
    return 1;
}

int TerminalView::cursorCallback(const VTermPos position, VTermPos, const int visible,
                                 void* user) {
    auto* view = static_cast<TerminalView*>(user);
    view->cursor_ = position;
    view->cursorVisible_ = visible != 0;
    view->cursorBlinkOn_ = true;
    return 1;
}

int TerminalView::propertyCallback(const VTermProp property, VTermValue* value, void* user) {
    auto* view = static_cast<TerminalView*>(user);
    if (property == VTERM_PROP_ALTSCREEN) {
        view->alternateScreen_ = value->boolean != 0;
        view->updateScrollBar(true);
    } else if (property == VTERM_PROP_CURSORVISIBLE) {
        view->cursorVisible_ = value->boolean != 0;
    } else if (property == VTERM_PROP_TITLE) {
        const VTermStringFragment fragment = value->string;
        if (fragment.initial) {
            view->pendingTitle_.clear();
        }
        view->pendingTitle_.append(QString::fromUtf8(fragment.str,
                                                     static_cast<qsizetype>(fragment.len)));
        if (fragment.final) {
            emit view->titleChanged(view->pendingTitle_);
        }
    }
    return 1;
}

int TerminalView::bellCallback(void* user) {
    QApplication::beep();
    Q_UNUSED(user)
    return 1;
}

int TerminalView::resizeCallback(int, int, void* user) {
    static_cast<TerminalView*>(user)->viewport()->update();
    return 1;
}

int TerminalView::scrollbackPushCallback(const int columns, const VTermScreenCell* cells,
                                         void* user) {
    auto* view = static_cast<TerminalView*>(user);
    const bool wasAtBottom =
        view->verticalScrollBar()->value() == view->verticalScrollBar()->maximum();
    QVector<VTermScreenCell> line(columns);
    std::memcpy(line.data(), cells,
                static_cast<size_t>(columns) * sizeof(VTermScreenCell));
    view->history_.append(std::move(line));
    if (view->history_.size() > maximumScrollbackLines) {
        view->history_.remove(0, view->history_.size() - maximumScrollbackLines);
    }
    view->updateScrollBar(wasAtBottom);
    return 1;
}

int TerminalView::scrollbackPopCallback(const int columns, VTermScreenCell* cells,
                                        void* user) {
    auto* view = static_cast<TerminalView*>(user);
    if (view->history_.isEmpty()) {
        return 0;
    }
    const QVector<VTermScreenCell> line = view->history_.takeLast();
    std::memset(cells, 0, static_cast<size_t>(columns) * sizeof(VTermScreenCell));
    std::memcpy(cells, line.constData(),
                static_cast<size_t>(std::min(columns, static_cast<int>(line.size()))) *
                    sizeof(VTermScreenCell));
    view->updateScrollBar(true);
    return 1;
}

int TerminalView::scrollbackClearCallback(void* user) {
    auto* view = static_cast<TerminalView*>(user);
    view->history_.clear();
    view->updateScrollBar(true);
    return 1;
}

void TerminalView::consumeOutput(const QByteArray& bytes) {
    trackSynchronizedOutput(bytes);
    const bool wasAtBottom = verticalScrollBar()->value() == verticalScrollBar()->maximum();
    vterm_input_write(terminal_, bytes.constData(), static_cast<size_t>(bytes.size()));
    vterm_screen_flush_damage(screen_);
    updateScrollBar(wasAtBottom);
    if (!synchronizedOutput_) {
        requestRender();
    }
}

void TerminalView::trackSynchronizedOutput(const QByteArray& bytes) {
    static const QByteArray beginSequence("\x1b[?2026h");
    static const QByteArray endSequence("\x1b[?2026l");
    const qsizetype maximumSequenceLength =
        std::max(beginSequence.size(), endSequence.size());

    for (const char byte : bytes) {
        synchronizedOutputScan_.append(byte);
        if (synchronizedOutputScan_.endsWith(beginSequence)) {
            synchronizedOutput_ = true;
            synchronizedOutputScan_.clear();
            synchronizedOutputTimer_->start();
            continue;
        }
        if (synchronizedOutputScan_.endsWith(endSequence)) {
            synchronizedOutput_ = false;
            synchronizedOutputScan_.clear();
            synchronizedOutputTimer_->stop();
            continue;
        }
        if (synchronizedOutputScan_.size() > maximumSequenceLength) {
            synchronizedOutputScan_.remove(
                0, synchronizedOutputScan_.size() - maximumSequenceLength);
        }
    }
    if (synchronizedOutput_) {
        synchronizedOutputTimer_->start();
    }
}

void TerminalView::requestRender() {
    renderPending_ = true;
    if (!renderTimer_->isActive()) {
        // The first frame remains low-latency; subsequent streaming output is
        // coalesced to one update per display frame.
        renderPending_ = false;
        viewport()->update();
        renderTimer_->start();
    }
}

void TerminalView::updateGeometryFromViewport() {
    const QFontMetrics metrics(font());
    cellWidth_ = std::max(1, metrics.horizontalAdvance(QLatin1Char('M')));
    cellHeight_ = std::max(1, metrics.height() + 2);
    const int availableWidth = std::max(1, viewport()->width() - leftPadding_ * 2);
    const int availableHeight = std::max(1, viewport()->height() - topPadding_ * 2);
    const int nextColumns = std::max(2, availableWidth / cellWidth_);
    const int nextRows = std::max(2, availableHeight / cellHeight_);
    if (nextRows == rows_ && nextColumns == columns_) {
        return;
    }
    rows_ = nextRows;
    columns_ = nextColumns;
    vterm_set_size(terminal_, rows_, columns_);
    process_.resizeTerminal(rows_, columns_);
    updateScrollBar(true);
}

void TerminalView::updateScrollBar(const bool preserveBottom) {
    QScrollBar* bar = verticalScrollBar();
    const int maximum = alternateScreen_ ? 0 : history_.size();
    bar->setRange(0, maximum);
    bar->setPageStep(rows_);
    if (preserveBottom) {
        bar->setValue(maximum);
    }
}

void TerminalView::sendKey(const VTermKey key, const VTermModifier modifiers) {
    vterm_keyboard_key(terminal_, key, modifiers);
}

VTermModifier TerminalView::modifiersFor(QKeyEvent* event) const {
    int modifiers = VTERM_MOD_NONE;
    if ((event->modifiers() & Qt::ShiftModifier) != 0) {
        modifiers |= VTERM_MOD_SHIFT;
    }
    if ((event->modifiers() & Qt::AltModifier) != 0) {
        modifiers |= VTERM_MOD_ALT;
    }
#if defined(Q_OS_MACOS)
    if ((event->modifiers() & Qt::MetaModifier) != 0) {
#else
    if ((event->modifiers() & Qt::ControlModifier) != 0) {
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
    vterm_screen_convert_color_to_rgb(screen_, &color);
    return QColor(color.rgb.red, color.rgb.green, color.rgb.blue);
}

VTermScreenCell TerminalView::cellAtVisiblePosition(const int displayRow,
                                                    const int column) const {
    VTermScreenCell cell {};
    cell.fg.type = VTERM_COLOR_DEFAULT_FG;
    cell.bg.type = VTERM_COLOR_DEFAULT_BG;
    const int absoluteRow = historyOffset() + displayRow;
    if (!alternateScreen_ && absoluteRow < history_.size()) {
        const QVector<VTermScreenCell>& line = history_.at(absoluteRow);
        if (column < line.size()) {
            return line.at(column);
        }
        return cell;
    }

    const int screenRow = alternateScreen_ ? displayRow : absoluteRow - history_.size();
    if (screenRow >= 0 && screenRow < rows_) {
        vterm_screen_get_cell(screen_, VTermPos{screenRow, column}, &cell);
    }
    return cell;
}

QString TerminalView::textForCell(const VTermScreenCell& cell) const {
    int length = 0;
    while (length < VTERM_MAX_CHARS_PER_CELL && cell.chars[length] != 0U) {
        ++length;
    }
    return length == 0
               ? QString()
               : QString::fromUcs4(reinterpret_cast<const char32_t*>(cell.chars), length);
}

int TerminalView::historyOffset() const {
    return alternateScreen_ ? 0 : verticalScrollBar()->value();
}

} // namespace ketplus

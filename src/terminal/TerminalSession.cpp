#include "terminal/TerminalSession.h"

#include <QDir>
#include <QTimer>

#include <algorithm>
#include <cstring>

namespace ketplus {
namespace {

constexpr int maximumScrollbackLines = 5000;

} // namespace

TerminalSession::TerminalSession(QObject* parent)
    : QObject(parent), process_(), terminal_(vterm_new(24, 80)),
      screen_(vterm_obtain_screen(terminal_)), synchronizedOutputTimer_(new QTimer(this)) {
    static const VTermScreenCallbacks callbacks = {
        &TerminalSession::damageCallback,
        nullptr,
        &TerminalSession::cursorCallback,
        &TerminalSession::propertyCallback,
        &TerminalSession::bellCallback,
        &TerminalSession::resizeCallback,
        &TerminalSession::scrollbackPushCallback,
        &TerminalSession::scrollbackPopCallback,
        &TerminalSession::scrollbackClearCallback,
    };

    vterm_set_utf8(terminal_, 1);
    vterm_output_set_callback(terminal_, &TerminalSession::outputCallback, this);
    vterm_screen_set_callbacks(screen_, &callbacks, this);
    vterm_screen_set_damage_merge(screen_, VTERM_DAMAGE_ROW);
    vterm_screen_enable_altscreen(screen_, 1);
    vterm_screen_enable_reflow(screen_, true);
    vterm_screen_reset(screen_, 1);

    synchronizedOutputTimer_->setSingleShot(true);
    synchronizedOutputTimer_->setInterval(120);
    connect(synchronizedOutputTimer_, &QTimer::timeout, this, [this] {
        synchronizedOutput_ = false;
        synchronizedOutputScan_.clear();
        emitContentChangedWhenReady();
    });

    connect(&process_, &PtyProcess::outputReceived, this, [this](const QByteArray& bytes) {
        emit outputReceived(bytes);
        feedOutput(bytes);
    });
    connect(&process_, &PtyProcess::errorOccurred, this, &TerminalSession::statusMessageRequested);
    connect(&process_, &PtyProcess::started, this, &TerminalSession::started);
    connect(&process_, &PtyProcess::exited, this, [this](const int exitCode) {
        emit statusMessageRequested(
            QStringLiteral("Terminal process exited with code %1").arg(exitCode));
        emit exited(exitCode);
        emit contentChanged();
    });
}

TerminalSession::~TerminalSession() {
    if (terminal_ != nullptr) {
        vterm_free(terminal_);
    }
}

bool TerminalSession::isRunning() const noexcept { return process_.isRunning(); }

QString TerminalSession::shellPath() const { return process_.shellPath(); }

int TerminalSession::rows() const noexcept { return rows_; }

int TerminalSession::columns() const noexcept { return columns_; }

int TerminalSession::historyLineCount() const noexcept { return history_.size(); }

int TerminalSession::contentLineCount() const noexcept {
    return alternateScreen_ ? rows_ : history_.size() + rows_;
}

bool TerminalSession::alternateScreen() const noexcept { return alternateScreen_; }

bool TerminalSession::synchronizedOutput() const noexcept { return synchronizedOutput_; }

bool TerminalSession::mouseTrackingEnabled() const noexcept {
    return mouseMode_ != VTERM_PROP_MOUSE_NONE;
}

VTermPos TerminalSession::cursorPosition() const noexcept { return cursor_; }

bool TerminalSession::cursorVisible() const noexcept { return cursorVisible_; }

bool TerminalSession::start(const QString& workingDirectory) {
    workingDirectory_ =
        QDir::cleanPath(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory);
    return process_.start(workingDirectory_, rows_, columns_);
}

bool TerminalSession::restart() {
    if (workingDirectory_.isEmpty()) {
        return false;
    }
    process_.stop();
    reset();
    return process_.start(workingDirectory_, rows_, columns_);
}

void TerminalSession::stop() { process_.stop(); }

void TerminalSession::resizeTerminal(const int rows, const int columns) {
    const int nextRows = std::max(2, rows);
    const int nextColumns = std::max(2, columns);
    if (nextRows == rows_ && nextColumns == columns_) {
        return;
    }
    rows_ = nextRows;
    columns_ = nextColumns;
    vterm_set_size(terminal_, rows_, columns_);
    process_.resizeTerminal(rows_, columns_);
    emit contentChanged();
}

void TerminalSession::reset() {
    synchronizedOutputTimer_->stop();
    synchronizedOutput_ = false;
    contentChangePending_ = false;
    synchronizedOutputScan_.clear();
    history_.clear();
    pendingTitle_.clear();
    cursor_ = VTermPos{0, 0};
    cursorVisible_ = true;
    alternateScreen_ = false;
    mouseMode_ = VTERM_PROP_MOUSE_NONE;
    vterm_screen_reset(screen_, 1);
    emit contentChanged();
}

void TerminalSession::clearScrollback() {
    if (history_.isEmpty()) {
        return;
    }
    history_.clear();
    emit contentChanged();
}

void TerminalSession::sendKey(const VTermKey key, const VTermModifier modifiers) {
    vterm_keyboard_key(terminal_, key, modifiers);
}

void TerminalSession::sendCodepoint(const uint character, const VTermModifier modifiers) {
    vterm_keyboard_unichar(terminal_, character, modifiers);
}

void TerminalSession::sendBytes(const QByteArray& bytes) {
    if (bytes.isEmpty()) {
        return;
    }
    emit inputGenerated(bytes);
    process_.send(bytes);
}

void TerminalSession::sendPaste(const QString& text) {
    if (text.isEmpty()) {
        return;
    }
    vterm_keyboard_start_paste(terminal_);
    sendBytes(text.toUtf8());
    vterm_keyboard_end_paste(terminal_);
}

void TerminalSession::sendMouseMove(const int row, const int column,
                                    const VTermModifier modifiers) {
    if (mouseTrackingEnabled()) {
        vterm_mouse_move(terminal_, std::clamp(row, 0, rows_ - 1),
                         std::clamp(column, 0, columns_ - 1), modifiers);
    }
}

void TerminalSession::sendMouseButton(const int button, const bool pressed,
                                      const VTermModifier modifiers) {
    if (mouseTrackingEnabled()) {
        vterm_mouse_button(terminal_, button, pressed, modifiers);
    }
}

void TerminalSession::setFocused(const bool focused) {
    if (focused) {
        vterm_state_focus_in(vterm_obtain_state(terminal_));
    } else {
        vterm_state_focus_out(vterm_obtain_state(terminal_));
    }
}

void TerminalSession::feedOutput(const QByteArray& bytes) {
    if (bytes.isEmpty()) {
        return;
    }
    trackSynchronizedOutput(bytes);
    vterm_input_write(terminal_, bytes.constData(), static_cast<size_t>(bytes.size()));
    vterm_screen_flush_damage(screen_);
    contentChangePending_ = true;
    emitContentChangedWhenReady();
}

VTermScreenCell TerminalSession::cellAt(const int absoluteRow, const int column) const {
    VTermScreenCell cell{};
    cell.fg.type = VTERM_COLOR_DEFAULT_FG;
    cell.bg.type = VTERM_COLOR_DEFAULT_BG;
    if (absoluteRow < 0 || column < 0 || column >= columns_) {
        return cell;
    }
    if (!alternateScreen_ && absoluteRow < history_.size()) {
        const QVector<VTermScreenCell>& line = history_.at(absoluteRow);
        return column < line.size() ? line.at(column) : cell;
    }
    const int screenRow = alternateScreen_ ? absoluteRow : absoluteRow - history_.size();
    if (screenRow >= 0 && screenRow < rows_) {
        vterm_screen_get_cell(screen_, VTermPos{screenRow, column}, &cell);
    }
    return cell;
}

QString TerminalSession::textForCell(const VTermScreenCell& cell) const {
    int length = 0;
    while (length < VTERM_MAX_CHARS_PER_CELL && cell.chars[length] != 0U) {
        ++length;
    }
    return length == 0 ? QString()
                       : QString::fromUcs4(reinterpret_cast<const char32_t*>(cell.chars), length);
}

QString TerminalSession::lineText(const int absoluteRow, const bool trimTrailing) const {
    QString line;
    for (int column = 0; column < columns_; ++column) {
        const QString cellText = textForCell(cellAt(absoluteRow, column));
        line.append(cellText.isEmpty() ? QStringLiteral(" ") : cellText);
    }
    if (trimTrailing) {
        while (line.endsWith(QLatin1Char(' '))) {
            line.chop(1);
        }
    }
    return line;
}

void TerminalSession::setDefaultColors(const int foregroundRed, const int foregroundGreen,
                                       const int foregroundBlue, const int backgroundRed,
                                       const int backgroundGreen, const int backgroundBlue) {
    VTermColor foreground;
    VTermColor background;
    vterm_color_rgb(&foreground, static_cast<uint8_t>(foregroundRed),
                    static_cast<uint8_t>(foregroundGreen), static_cast<uint8_t>(foregroundBlue));
    vterm_color_rgb(&background, static_cast<uint8_t>(backgroundRed),
                    static_cast<uint8_t>(backgroundGreen), static_cast<uint8_t>(backgroundBlue));
    vterm_screen_set_default_colors(screen_, &foreground, &background);
    emit contentChanged();
}

void TerminalSession::convertColorToRgb(VTermColor& color) const {
    vterm_screen_convert_color_to_rgb(screen_, &color);
}

void TerminalSession::outputCallback(const char* bytes, const size_t length, void* user) {
    auto* session = static_cast<TerminalSession*>(user);
    const QByteArray output(bytes, static_cast<qsizetype>(length));
    emit session->inputGenerated(output);
    session->process_.send(output);
}

int TerminalSession::damageCallback(VTermRect, void*) { return 1; }

int TerminalSession::cursorCallback(const VTermPos position, VTermPos, const int visible,
                                    void* user) {
    auto* session = static_cast<TerminalSession*>(user);
    session->cursor_ = position;
    session->cursorVisible_ = visible != 0;
    return 1;
}

int TerminalSession::propertyCallback(const VTermProp property, VTermValue* value, void* user) {
    auto* session = static_cast<TerminalSession*>(user);
    if (property == VTERM_PROP_ALTSCREEN) {
        const bool enabled = value->boolean != 0;
        if (session->alternateScreen_ != enabled) {
            session->alternateScreen_ = enabled;
            emit session->alternateScreenChanged(enabled);
        }
    } else if (property == VTERM_PROP_CURSORVISIBLE) {
        session->cursorVisible_ = value->boolean != 0;
    } else if (property == VTERM_PROP_MOUSE) {
        session->mouseMode_ = value->number;
    } else if (property == VTERM_PROP_TITLE) {
        const VTermStringFragment fragment = value->string;
        if (fragment.initial) {
            session->pendingTitle_.clear();
        }
        session->pendingTitle_.append(
            QString::fromUtf8(fragment.str, static_cast<qsizetype>(fragment.len)));
        if (fragment.final) {
            emit session->titleChanged(session->pendingTitle_);
        }
    }
    return 1;
}

int TerminalSession::bellCallback(void* user) {
    emit static_cast<TerminalSession*>(user)->bellRequested();
    return 1;
}

int TerminalSession::resizeCallback(int, int, void*) { return 1; }

int TerminalSession::scrollbackPushCallback(const int columns, const VTermScreenCell* cells,
                                            void* user) {
    auto* session = static_cast<TerminalSession*>(user);
    QVector<VTermScreenCell> line(columns);
    std::memcpy(line.data(), cells, static_cast<size_t>(columns) * sizeof(VTermScreenCell));
    session->history_.append(std::move(line));
    if (session->history_.size() > maximumScrollbackLines) {
        const int removed = session->history_.size() - maximumScrollbackLines;
        session->history_.remove(0, removed);
        emit session->historyTrimmed(removed);
    }
    return 1;
}

int TerminalSession::scrollbackPopCallback(const int columns, VTermScreenCell* cells, void* user) {
    auto* session = static_cast<TerminalSession*>(user);
    if (session->history_.isEmpty()) {
        return 0;
    }
    const QVector<VTermScreenCell> line = session->history_.takeLast();
    std::memset(cells, 0, static_cast<size_t>(columns) * sizeof(VTermScreenCell));
    std::memcpy(cells, line.constData(),
                static_cast<size_t>(std::min(columns, static_cast<int>(line.size()))) *
                    sizeof(VTermScreenCell));
    return 1;
}

int TerminalSession::scrollbackClearCallback(void* user) {
    static_cast<TerminalSession*>(user)->history_.clear();
    return 1;
}

void TerminalSession::trackSynchronizedOutput(const QByteArray& bytes) {
    static const QByteArray beginSequence("\x1b[?2026h");
    static const QByteArray endSequence("\x1b[?2026l");
    const qsizetype maximumSequenceLength = std::max(beginSequence.size(), endSequence.size());

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
            synchronizedOutputScan_.remove(0,
                                           synchronizedOutputScan_.size() - maximumSequenceLength);
        }
    }
    if (synchronizedOutput_) {
        synchronizedOutputTimer_->start();
    }
}

void TerminalSession::emitContentChangedWhenReady() {
    if (!synchronizedOutput_ && contentChangePending_) {
        contentChangePending_ = false;
        emit contentChanged();
    }
}

} // namespace ketplus

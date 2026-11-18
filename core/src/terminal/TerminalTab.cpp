#include "terminal/TerminalTab.h"
#include "terminal/RunCommand.h"

#include "terminal/TerminalView.h"
#include "ui/Theme.h"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {
namespace {

QString displayNameForDirectory(const QString& path) {
    if (QDir::cleanPath(path) == QDir::cleanPath(QDir::homePath())) {
        return QStringLiteral("~");
    }
    const QFileInfo directory(path);
    return directory.fileName().isEmpty() ? directory.absoluteFilePath() : directory.fileName();
}

} // namespace

TerminalTab::TerminalTab(const QString& workingDirectory, const QString& commandPath,
                         QWidget* parent)
    : QWidget(parent), session_(this), terminal_(new TerminalView(session_, this)),
      workingDirectory_(
          QDir::cleanPath(workingDirectory.isEmpty() ? QDir::homePath() : workingDirectory)),
      pendingCommandPath_(commandPath), title_(displayNameForDirectory(workingDirectory_)) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(terminal_);

    connect(terminal_, &TerminalView::titleChanged, this, &TerminalTab::setTitle);
    connect(terminal_, &TerminalView::statusMessageRequested, this,
            &TerminalTab::statusMessageRequested);
    connect(&session_, &TerminalSession::started, this, [this] {
        setTitle({});
        if (!pendingCommandPath_.isEmpty()) {
            if (const QByteArray command = commandForPath(std::exchange(pendingCommandPath_, {}));
                !command.isEmpty()) {
                // PowerShell's line editor drops what arrives before its first prompt; the
                // other shells keep it in the terminal's input queue.
                if (shellKindFor(session_.shellPath()) == ShellKind::PowerShell)
                    pendingRun_ = command;
                else
                    session_.sendBytes(command);
            }
        }
        shellReady_ = false;
        sinceStart_.start();
        typePendingTextWhenReady();
    });
    // The shell is ready once its prompt has been drawn and output has paused.
    promptSettled_.setSingleShot(true);
    promptSettled_.setInterval(150);
    connect(&session_, &TerminalSession::outputReceived, this, [this] {
        if (shellReady_)
            return;
        // A command waiting for PowerShell needs its prompt: before it, the line editor is
        // still loading and drops input, even if the banner paused. Without a prompt the
        // wait in typePendingTextWhenReady() ends it.
        if (!pendingRun_.isEmpty() && !powerShellPromptVisible())
            return;
        promptSettled_.start();
    });
    connect(&promptSettled_, &QTimer::timeout, this, [this] {
        shellReady_ = true;
        if (!pendingRun_.isEmpty())
            session_.sendBytes(std::exchange(pendingRun_, {}));
        if (!pendingTypedText_.isEmpty())
            session_.sendPaste(std::exchange(pendingTypedText_, {}));
    });
    connect(&session_, &TerminalSession::exited, this,
            [this](const int exitCode) { setTitle(QStringLiteral("Exited (%1)").arg(exitCode)); });
}

TerminalTab::~TerminalTab() { stop(); }

bool TerminalTab::powerShellPromptVisible() const {
    // The screen ends with a prompt such as `PS C:\Users\me>`. A long prompt wraps, so the
    // rows are joined first.
    static const QRegularExpression prompt(QStringLiteral(R"(PS [^>]*>\s*$)"));
    QString text = terminal_->visibleText();
    text.remove(QLatin1Char('\n'));
    return prompt.match(text).hasMatch();
}

QString TerminalTab::workingDirectory() const { return workingDirectory_; }

QString TerminalTab::title() const { return title_; }

TerminalView* TerminalTab::view() const noexcept { return terminal_; }

void TerminalTab::setDirectoryBeforeStart(const QString& path) {
    if (started_)
        return;
    workingDirectory_ = QDir::cleanPath(path.isEmpty() ? QDir::homePath() : path);
    setTitle({});
}

void TerminalTab::start() {
    if (started_) {
        terminal_->setFocus(Qt::ShortcutFocusReason);
        return;
    }
    started_ = true;
    QTimer::singleShot(0, this, [this] { terminal_->start(workingDirectory_); });
}

bool TerminalTab::restart() {
    pendingCommandPath_.clear();
    pendingRun_.clear();
    return terminal_->restart();
}

void TerminalTab::stop() { session_.stop(); }

void TerminalTab::typeText(const QString& text) {
    // Typed, never run: a line break or another control character would run or edit it, since
    // a paste is only bracketed when the shell asks for it.
    QString typed;
    typed.reserve(text.size());
    for (const QChar character : text)
        if (character.unicode() >= 0x20 && character.unicode() != 0x7f &&
            character.category() != QChar::Other_Control)
            typed.append(character);
    if (session_.isRunning() && shellReady_) {
        session_.sendPaste(typed);
        return;
    }
    pendingTypedText_ = typed;
    if (session_.isRunning())
        typePendingTextWhenReady();
}

void TerminalTab::typePendingTextWhenReady() {
    if (shellReady_ || (pendingTypedText_.isEmpty() && pendingRun_.isEmpty()))
        return;
    // A shell that prints nothing still gets the text, after at most three seconds.
    constexpr qint64 longestWaitMs = 3000;
    const qint64 left = longestWaitMs - (sinceStart_.isValid() ? sinceStart_.elapsed() : 0);
    QTimer::singleShot(qMax<qint64>(0, left), this, [this] {
        if (!shellReady_ && (!pendingTypedText_.isEmpty() || !pendingRun_.isEmpty()))
            promptSettled_.start(0);
    });
}

void TerminalTab::applyTheme(const ThemePalette& palette) { terminal_->applyTheme(palette); }
void TerminalTab::applyColorScheme(const ColorScheme& scheme) {
    terminal_->applyColorScheme(scheme);
}

void TerminalTab::setTypography(const int fontSizePixels, const int lineHeightPixels,
                                const QString& fontFamily) {
    terminal_->setTypography(fontSizePixels, lineHeightPixels, fontFamily);
}

void TerminalTab::setTitle(const QString& title) {
    QString normalized = title.simplified().left(120);
    if (normalized.isEmpty()) {
        normalized = displayNameForDirectory(workingDirectory_);
    }
    if (title_ == normalized) {
        return;
    }
    title_ = normalized;
    emit titleChanged(title_);
}

QByteArray TerminalTab::commandForPath(const QString& path) {
    const QFileInfo file(path);
    const QString shell = session_.shellPath();
    const RunCommand command =
        runCommandFor(shellKindFor(shell), shell, file.absoluteFilePath(), file.isExecutable());
    if (!command.error.isEmpty()) {
        emit statusMessageRequested(command.error);
        return {};
    }
    return (command.line + QLatin1Char('\r')).toUtf8();
}

} // namespace ketplus

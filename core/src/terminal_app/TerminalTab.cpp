#include "terminal_app/TerminalTab.h"

#include "terminal/TerminalView.h"
#include "ui/Theme.h"

#include <QDir>
#include <QFileInfo>
#include <QTimer>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {
namespace {

QString shellQuote(QString value) {
    value.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'%1'").arg(value);
}

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
            session_.sendBytes(commandForPath(std::exchange(pendingCommandPath_, {})));
        }
    });
    connect(&session_, &TerminalSession::exited, this,
            [this](const int exitCode) { setTitle(QStringLiteral("Exited (%1)").arg(exitCode)); });
}

TerminalTab::~TerminalTab() { stop(); }

QString TerminalTab::workingDirectory() const { return workingDirectory_; }

QString TerminalTab::title() const { return title_; }

TerminalView* TerminalTab::view() const noexcept { return terminal_; }

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
    return terminal_->restart();
}

void TerminalTab::stop() { session_.stop(); }

void TerminalTab::applyTheme(const ThemePalette& palette) { terminal_->applyTheme(palette); }

void TerminalTab::setTypography(const int fontSizePixels, const int lineHeightPixels) {
    terminal_->setTypography(fontSizePixels, lineHeightPixels);
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

QByteArray TerminalTab::commandForPath(const QString& path) const {
    const QFileInfo file(path);
    QString command;
    if (file.isExecutable()) {
        command = shellQuote(file.absoluteFilePath());
    } else {
        const QString shell =
            session_.shellPath().isEmpty() ? QStringLiteral("/bin/sh") : session_.shellPath();
        command =
            QStringLiteral("%1 %2").arg(shellQuote(shell), shellQuote(file.absoluteFilePath()));
    }
    return (command + QLatin1Char('\r')).toUtf8();
}

} // namespace ketplus

#include "app/AtlasConversationPanel.h"

#include "atlas/ManagedAtlasViewers.h"
#include "ui/TypeScale.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace ketplus {

AtlasConversationPanel::AtlasConversationPanel(AtlasIndex* index, QWidget* parent)
    : QWidget(parent), index_(index) {
    setObjectName(QStringLiteral("conversationAtlasPanel"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 16, 20, 16);
    layout->setSpacing(12);
    auto* heading = new QHBoxLayout;
    auto* title = new QLabel(tr("Atlases in this worktree"), this);
    title->setObjectName(QStringLiteral("conversationAtlasTitle"));
    heading->addWidget(title, 1);
    auto* create = new QPushButton(tr("New Atlas"), this);
    create->setObjectName(QStringLiteral("conversationAtlasNew"));
    connect(create, &QPushButton::clicked, this, &AtlasConversationPanel::newRequested);
    heading->addWidget(create);
    layout->addLayout(heading);
    search_ = new QLineEdit(this);
    search_->setObjectName(QStringLiteral("conversationAtlasSearch"));
    search_->setPlaceholderText(tr("Search Atlases"));
    search_->setAccessibleName(tr("Search Atlases"));
    layout->addWidget(search_);
    count_ = new QLabel(this);
    count_->setObjectName(QStringLiteral("conversationAtlasCount"));
    layout->addWidget(count_);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("conversationAtlasScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    rows_ = new QWidget(scroll);
    rowsLayout_ = new QVBoxLayout(rows_);
    rowsLayout_->setContentsMargins(0, 0, 0, 0);
    rowsLayout_->setSpacing(8);
    scroll->setWidget(rows_);
    layout->addWidget(scroll, 1);
    connect(search_, &QLineEdit::textChanged, this, &AtlasConversationPanel::render);
    connect(index_, &AtlasIndex::changed, this, &AtlasConversationPanel::render);
    connect(ManagedAtlasViewers::instance(), &ManagedAtlasViewers::changed, this, [this] {
        QMetaObject::invokeMethod(this, &AtlasConversationPanel::render, Qt::QueuedConnection);
    });
    applyPalette(palette());
    render();
}

void AtlasConversationPanel::applyPalette(const QPalette& colors) {
    const auto page = colors.color(QPalette::Window).name();
    const auto surface = colors.color(QPalette::Base).name();
    const auto text = colors.color(QPalette::WindowText).name();
    const auto muted = colors.color(QPalette::PlaceholderText).name();
    const auto border = colors.color(QPalette::Mid).name();
    const auto accent = colors.color(QPalette::Highlight).name();
    const int basePixels = QApplication::font().pixelSize() > 0
                               ? QApplication::font().pixelSize()
                               : InterfaceZoomMetrics::mediumPixels;
    setStyleSheet(TypeScale::apply(QStringLiteral(R"QSS(
QWidget#conversationAtlasPanel, QWidget#conversationAtlasScroll { background:%1; color:%3; }
QLabel#conversationAtlasTitle { font-weight:600; font-size:{{title}}px; }
QLabel#conversationAtlasCount, QLabel[atlasMeta="true"] { color:%4; }
QWidget[atlasRow="true"] { background:%2; border:1px solid %5; border-radius:7px; }
QLineEdit#conversationAtlasSearch { min-height:30px; background:%2; color:%3; border:1px solid %5; border-radius:5px; padding:0 8px; }
QLineEdit#conversationAtlasSearch:focus { border-color:%6; }
QPushButton { min-height:28px; padding:0 10px; border:1px solid %5; border-radius:5px; background:%2; color:%3; }
QPushButton:hover { border-color:%6; }
QPushButton:focus { border-color:%6; }
QPushButton#conversationAtlasNew { background:%6; border-color:%6; color:white; font-weight:600; }
)QSS")
                                       .arg(page, surface, text, muted, border, accent),
                                   basePixels));
}

void AtlasConversationPanel::render() {
    while (auto* item = rowsLayout_->takeAt(0)) {
        if (auto* widget = item->widget())
            delete widget;
        delete item;
    }
    const auto viewers = ManagedAtlasViewers::instance()->viewers();
    QSet<QString> localPaths;
    int localCount = 0;
    for (const auto& atlas : index_->atlases()) {
        localPaths.insert(atlas.path);
        if (!atlas.title.contains(search_->text(), Qt::CaseInsensitive) &&
            !atlas.relativePath.contains(search_->text(), Qt::CaseInsensitive))
            continue;
        addAtlasRow(rowsLayout_, atlas, false);
        ++localCount;
    }
    count_->setText(
        tr("%1 Atlas%2").arg(localCount).arg(localCount == 1 ? QString{} : QStringLiteral("es")));
    if (localCount == 0) {
        auto* empty =
            new QLabel(search_->text().isEmpty()
                           ? tr("No Atlases in this worktree. Ask your Agent to create one.")
                           : tr("No Atlases match your search."),
                       rows_);
        empty->setWordWrap(true);
        rowsLayout_->addWidget(empty);
    }
    bool headingAdded = false;
    for (const auto& viewer : viewers) {
        if (localPaths.contains(viewer.atlas.path) ||
            (!search_->text().isEmpty() &&
             !viewer.atlas.title.contains(search_->text(), Qt::CaseInsensitive)))
            continue;
        if (!headingAdded) {
            auto* heading = new QLabel(tr("Running in other worktrees"), rows_);
            heading->setObjectName(QStringLiteral("conversationAtlasElsewhere"));
            rowsLayout_->addWidget(heading);
            headingAdded = true;
        }
        addAtlasRow(rowsLayout_, viewer.atlas, true);
    }
    rowsLayout_->addStretch();
}

void AtlasConversationPanel::addAtlasRow(QVBoxLayout* layout, const AtlasDescriptor& atlas,
                                         const bool elsewhere) {
    auto* row = new QWidget(rows_);
    row->setProperty("atlasRow", true);
    row->setObjectName(QStringLiteral("conversationAtlasRow"));
    auto* body = new QVBoxLayout(row);
    body->setContentsMargins(12, 10, 12, 10);
    body->setSpacing(6);
    auto* title = new QLabel(atlas.title, row);
    title->setObjectName(QStringLiteral("conversationAtlasRowTitle"));
    title->setWordWrap(true);
    body->addWidget(title);
    auto* meta = new QLabel(elsewhere ? atlas.workspaceRoot : atlas.relativePath, row);
    meta->setProperty("atlasMeta", true);
    meta->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body->addWidget(meta);
    auto* actions = new QHBoxLayout;
    const auto viewers = ManagedAtlasViewers::instance()->viewers();
    auto viewer = std::find_if(viewers.begin(), viewers.end(), [&atlas](const auto& item) {
        return item.atlas.path == atlas.path;
    });
    const bool active = viewer != viewers.end();
    const bool ready = active && viewer->state == ManagedAtlasViewers::State::Running;
    const auto error = ManagedAtlasViewers::instance()->errorFor(atlas.path);
    if (active) {
        auto* status =
            new QLabel(ready                                                   ? tr("Running")
                       : viewer->state == ManagedAtlasViewers::State::Starting ? tr("Starting…")
                                                                               : tr("Stopping…"),
                       row);
        status->setProperty("atlasMeta", true);
        actions->addWidget(status);
    } else if (!error.isEmpty()) {
        auto* status = new QLabel(error, row);
        status->setProperty("atlasMeta", true);
        status->setWordWrap(true);
        body->addWidget(status);
        if (error.contains(QStringLiteral("0.6.1"))) {
            auto* install = new QPushButton(tr("Install KetAtlas 0.6.1"), row);
            install->setObjectName(QStringLiteral("conversationAtlasInstallCli"));
            connect(install, &QPushButton::clicked, this, [this, atlas] { installCli(atlas); });
            actions->addWidget(install);
        }
    }
    actions->addStretch();
    if (!elsewhere) {
        auto* edit = new QPushButton(tr("Edit"), row);
        edit->setObjectName(QStringLiteral("conversationAtlasEdit"));
        edit->setAccessibleName(tr("Edit %1 in KétPlus").arg(atlas.title));
        connect(edit, &QPushButton::clicked, this, [this, atlas] { emit editRequested(atlas); });
        actions->addWidget(edit);
        auto* open = new QPushButton(ready ? tr("Focus browser") : tr("Open in browser"), row);
        open->setObjectName(QStringLiteral("conversationAtlasOpen"));
        open->setEnabled(!active || ready);
        connect(open, &QPushButton::clicked, this,
                [atlas] { ManagedAtlasViewers::instance()->open(atlas); });
        actions->addWidget(open);
    }
    if (active) {
        auto* stop = new QPushButton(tr("Stop"), row);
        stop->setObjectName(QStringLiteral("conversationAtlasStop"));
        stop->setEnabled(viewer->state != ManagedAtlasViewers::State::Stopping);
        stop->setAccessibleName(tr("Stop viewer for %1").arg(atlas.title));
        connect(stop, &QPushButton::clicked, this,
                [atlas] { ManagedAtlasViewers::instance()->stop(atlas.path); });
        actions->addWidget(stop);
    }
    body->addLayout(actions);
    layout->addWidget(row);
}

void AtlasConversationPanel::installCli(const AtlasDescriptor& atlas) {
    if (installer_ && installer_->state() != QProcess::NotRunning)
        return;
    const auto npm = QStandardPaths::findExecutable(QStringLiteral("npm"));
    if (npm.isEmpty()) {
        QMessageBox::warning(this, tr("KetAtlas"),
                             tr("Install Node.js 22 or newer and npm, then try again."));
        return;
    }
    auto* process = new QProcess(this);
    installer_ = process;
    process->setProcessChannelMode(QProcess::MergedChannels);
    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"), QFileInfo(npm).absolutePath() +
                                                   QDir::listSeparator() +
                                                   environment.value(QStringLiteral("PATH")));
    process->setProcessEnvironment(environment);
    connect(process, &QProcess::finished, this,
            [this, process, npm, atlas](int code, QProcess::ExitStatus status) {
                if (installer_ == process)
                    installer_ = nullptr;
                const auto output = QString::fromUtf8(process->readAll()).right(700);
                process->deleteLater();
                if (code != 0 || status != QProcess::NormalExit) {
                    QMessageBox::warning(this, tr("KetAtlas installation failed"), output);
                    return;
                }
                const auto cli = QFileInfo(npm).absoluteDir().filePath(QStringLiteral("ketatlas"));
                if (QFileInfo(cli).isExecutable())
                    QSettings().setValue(QStringLiteral("atlas/cliPath"), cli);
                ManagedAtlasViewers::instance()->open(atlas);
            });
    connect(process, &QProcess::errorOccurred, this, [this, process](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            installer_ = nullptr;
            QMessageBox::warning(this, tr("KetAtlas installation failed"), process->errorString());
            process->deleteLater();
        }
    });
    process->start(
        npm, {QStringLiteral("install"), QStringLiteral("-g"), QStringLiteral("ketatlas@0.6.1")});
}

} // namespace ketplus

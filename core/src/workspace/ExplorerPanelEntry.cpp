#include "KetBanner.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetTextField.h"
#include "workspace/ExplorerPanel.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QTreeView>
#include <QVBoxLayout>
namespace ketplus {
void ExplorerPanel::createEntry(bool directory) {
    entryParent_ = targetDirectory();
    if (entryParent_.isEmpty())
        return;
    entryDirectory_ = directory;
    if (!entryRow_) {
        entryRow_ = new QWidget(this);
        entryRow_->setObjectName(QStringLiteral("explorerNewEntry"));
        auto* column = new QVBoxLayout(entryRow_);
        column->setContentsMargins(8, 4, 8, 4);
        entryLocation_ = new QLabel(entryRow_);
        column->addWidget(entryLocation_);
        auto* row = new QHBoxLayout;
        entryName_ = new ket::TextField(entryRow_);
        entryName_->setObjectName(QStringLiteral("explorerNewEntryName"));
        entryName_->setAccessibleName(tr("New file or folder name"));
        auto* cancel = new ket::IconButton(ket::Icons::icon(ket::Icons::codeForLucide(u"x")),
                                           tr("Cancel creation"), entryRow_);
        row->addWidget(entryName_, 1);
        row->addWidget(cancel);
        column->addLayout(row);
        entryError_ =
            new ket::Banner(ket::BannerTone::Danger, tr("Could not create item"), {}, entryRow_);
        column->addWidget(entryError_);
        static_cast<QVBoxLayout*>(layout())->insertWidget(2, entryRow_);
        connect(entryName_, &QLineEdit::returnPressed, this, &ExplorerPanel::commitNewEntry);
        connect(cancel, &QAbstractButton::clicked, entryRow_, &QWidget::hide);
        entryName_->installEventFilter(this);
    }
    entryLocation_->setText(QDir(rootPath_).relativeFilePath(entryParent_));
    entryLocation_->setToolTip(entryParent_);
    entryName_->setPlaceholderText(directory ? tr("Folder name · Enter to create")
                                             : tr("File name · Enter to create"));
    entryName_->clear();
    entryError_->hide();
    entryRow_->show();
    entryName_->setFocus();
}
void ExplorerPanel::commitNewEntry() {
    const QString name = entryName_->text().trimmed();
    const auto fail = [this](const QString& message) {
        entryError_->setMessage(message);
        entryError_->show();
    };
    if (name.isEmpty() || name == "." || name == ".." || name.contains('/') ||
        name.contains('\\')) {
        fail(tr("Enter a name without slashes."));
        return;
    }
    const QString path = QDir(entryParent_).filePath(name);
    if (QFileInfo::exists(path)) {
        fail(tr("An item with this name already exists."));
        return;
    }
    bool created;
    if (entryDirectory_)
        created = QDir(entryParent_).mkdir(name);
    else {
        QFile file(path);
        created = file.open(QIODevice::WriteOnly | QIODevice::NewOnly);
    }
    if (!created) {
        fail(tr("Check folder permissions and try again."));
        return;
    }
    entryRow_->hide();
    revealPath(path);
    if (!entryDirectory_)
        emit fileOpenRequested(path);
}
bool ExplorerPanel::eventFilter(QObject* watched, QEvent* event) {
    if (watched == entryName_ &&
        (event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress) &&
        static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
        if (event->type() == QEvent::KeyPress) {
            entryRow_->hide();
            tree_->setFocus();
        }
        event->accept();
        return true;
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace ketplus

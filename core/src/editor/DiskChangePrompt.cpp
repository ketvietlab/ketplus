#include "editor/DiskChangePrompt.h"

#include "editor/EditorWidget.h"

#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>

namespace ketplus::disk_change {

bool confirmSave(QWidget* parent, EditorWidget* editor) {
    const Document& document = editor->document();
    if (document.isUntitled() || !document.changedOnDisk()) {
        return true;
    }

    const bool exists = QFileInfo::exists(document.filePath());
    QMessageBox box(
        QMessageBox::Warning, QStringLiteral("File changed on disk"),
        exists ? QStringLiteral("%1 was changed on disk after you opened it.")
                     .arg(document.displayName())
               : QStringLiteral("%1 was deleted or moved on disk.").arg(document.displayName()),
        QMessageBox::NoButton, parent);
    box.setInformativeText(exists ? QStringLiteral("Saving replaces those changes with yours.")
                                  : QStringLiteral("Saving creates the file again."));
    QPushButton* overwrite = box.addButton(
        exists ? QStringLiteral("Overwrite") : QStringLiteral("Save"), QMessageBox::AcceptRole);
    QPushButton* reload = exists ? box.addButton(QStringLiteral("Reload and Discard My Edits"),
                                                 QMessageBox::DestructiveRole)
                                 : nullptr;
    QPushButton* cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.exec();

    if (box.clickedButton() == overwrite) {
        return true;
    }
    if (reload != nullptr && box.clickedButton() == reload) {
        if (const auto result = editor->reloadFromDisk(); !result.ok) {
            QMessageBox::critical(parent, QStringLiteral("Unable to reload file"), result.error);
        }
    }
    return false;
}

void refreshCleanTabs(QTabWidget* tabs) {
    for (int index = 0; index < tabs->count(); ++index) {
        auto* editor = qobject_cast<EditorWidget*>(tabs->widget(index));
        if (editor == nullptr || editor->isHibernated() || editor->document().isModified() ||
            !editor->document().changedOnDisk()) {
            continue;
        }
        // A deleted file keeps its last text; saving it asks first.
        (void)editor->refreshFromDisk();
    }
}

} // namespace ketplus::disk_change

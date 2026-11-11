#include "editor/DiskChangePrompt.h"

#include "editor/EditorWidget.h"

#include "ui/MessageDialog.h"
#include <QFileInfo>
#include <QPushButton>
#include <QTabWidget>

namespace ketplus::disk_change {

bool restoreBeforeSave(QWidget* parent, EditorWidget* editor) {
    if (!editor->isHibernated()) {
        return true;
    }
    if (const auto result = editor->restoreFromDisk(); !result.ok) {
        message_dialog::critical(parent, QStringLiteral("Unable to save file"),
                                 QStringLiteral("%1 has not been read back since its tab was "
                                                "unloaded, so there is nothing to save.\n\n%2")
                                     .arg(editor->document().displayName(), result.error));
        return false;
    }
    return true;
}

bool confirmSave(QWidget* parent, EditorWidget* editor) {
    const Document& document = editor->document();
    if (document.isUntitled() || !document.changedOnDisk()) {
        return true;
    }

    const bool exists = QFileInfo::exists(document.filePath());
    const QString message =
        (exists
             ? QStringLiteral("%1 was changed on disk after you opened it.\n\nSaving replaces "
                              "those changes with yours.")
             : QStringLiteral("%1 was deleted or moved on disk.\n\nSaving creates the file again."))
            .arg(document.displayName());
    QList<message_dialog::Choice> choices = {
        {QMessageBox::Save, exists ? QStringLiteral("Overwrite") : QStringLiteral("Save")}};
    if (exists)
        choices.append({QMessageBox::Discard, QStringLiteral("Reload and Discard My Edits"), true});
    choices.append({QMessageBox::Cancel, QStringLiteral("Cancel")});
    const auto answer =
        message_dialog::choose(parent, QStringLiteral("File changed on disk"), message, choices);
    if (answer == QMessageBox::Save)
        return true;
    if (exists && answer == QMessageBox::Discard) {
        if (const auto result = editor->reloadFromDisk(); !result.ok) {
            message_dialog::critical(parent, QStringLiteral("Unable to reload file"), result.error);
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

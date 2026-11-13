#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetTextField.h"
#include "app/theme/SchemeLibraryPanel.h"
#include "ui/theme/ThemeSelection.h"
#include <QFileDialog>
#include <QLabel>
#include <QVBoxLayout>

namespace ketplus {
void SchemeLibraryPanel::duplicateSelected() {
    const auto original = library_.catalog().load(selectedId_);
    if (!original.success) {
        showError(SchemeLibrary::diagnosticText(original.diagnostics));
        return;
    }
    auto* dialog = new ket::Dialog(tr("Duplicate color scheme"), this);
    connect(this, &QObject::destroyed, dialog, &ket::Dialog::reject);
    dialog->setObjectName(QStringLiteral("duplicateSchemeDialog"));
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* name = new ket::TextField(body);
    name->setObjectName(QStringLiteral("duplicateSchemeName"));
    name->setText(tr("%1 copy").arg(original.scheme.name));
    name->setAccessibleName(tr("Scheme name"));
    name->selectAll();
    auto* field = new ket::FieldRow(
        tr("Name"), name,
        tr("Creates a separate .ketscheme.json file in your color scheme library."), body);
    field->setStacked(true);
    layout->addWidget(field);
    dialog->setBody(body);
    auto* cancel = dialog->addButton(tr("Cancel"));
    auto* accept = dialog->addButton(tr("Duplicate"), ket::ButtonVariant::Primary);
    connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    connect(name, &QLineEdit::textChanged, accept,
            [accept](const QString& text) { accept->setEnabled(!text.trimmed().isEmpty()); });
    connect(accept, &QAbstractButton::clicked, this, [this, dialog, name, field, id = selectedId_] {
        const auto result = library_.catalog().duplicate(id, name->text().trimmed());
        if (!result.success) {
            field->setError(SchemeLibrary::diagnosticText(result.diagnostics));
            return;
        }
        const bool selected = selectScheme(result.scheme.id);
        if (selected)
            selectedId_ = result.scheme.id;
        reload();
        emit catalogChanged();
        dialog->accept();
        if (!selected)
            showError(tr("The copy was saved, but could not be selected: ") +
                      selection_->lastError());
    });
    connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    dialog->open();
    name->setFocus();
}
void SchemeLibraryPanel::exportSelected() {
    QFileDialog chooser(this, tr("Export color scheme"));
    chooser.setAcceptMode(QFileDialog::AcceptSave);
    chooser.setNameFilter(tr("KetPlus color scheme (*.ketscheme.json)"));
    chooser.setDefaultSuffix(QStringLiteral("ketscheme.json"));
    chooser.selectFile(QStringLiteral("scheme.ketscheme.json"));
    if (chooser.exec() != QDialog::Accepted || chooser.selectedFiles().isEmpty())
        return;
    const auto path = chooser.selectedFiles().first();
    if (!path.endsWith(u".ketscheme.json", Qt::CaseInsensitive)) {
        showError(tr("Choose a filename ending in .ketscheme.json."));
        return;
    }
    QList<SchemeDiagnostic> diagnostics;
    if (!library_.exportScheme(selectedId_, path, &diagnostics))
        showError(SchemeLibrary::diagnosticText(diagnostics));
}
void SchemeLibraryPanel::deleteSelected() {
    if (selectedId_.startsWith(u"builtin-"))
        return;
    const auto original = library_.catalog().load(selectedId_);
    if (!original.success) {
        showError(SchemeLibrary::diagnosticText(original.diagnostics));
        return;
    }
    auto* dialog = new ket::Dialog(tr("Delete color scheme?"), this);
    connect(this, &QObject::destroyed, dialog, &ket::Dialog::reject);
    auto* description = new QLabel(tr("Delete “%1” from your library? This cannot be undone. "
                                      "If it is in use, KetPlus will return to a built-in scheme.")
                                       .arg(original.scheme.name));
    description->setTextFormat(Qt::PlainText);
    description->setWordWrap(true);
    dialog->setBody(description);
    auto* cancel = dialog->addButton(tr("Cancel"));
    auto* accept = dialog->addButton(tr("Delete"), ket::ButtonVariant::Danger);
    accept->setObjectName(QStringLiteral("confirmDeleteScheme"));
    connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    connect(accept, &QAbstractButton::clicked, this, [this, dialog, id = selectedId_] {
        QList<SchemeDiagnostic> diagnostics;
        const auto current = library_.catalog().load(id);
        if (!current.success) {
            dialog->reject();
            reload();
            showError(SchemeLibrary::diagnosticText(current.diagnostics));
            return;
        }
        if (!prepareRemoval(current.scheme)) {
            dialog->reject();
            return;
        }
        if (!library_.catalog().remove(id, &diagnostics)) {
            dialog->reject();
            showError(SchemeLibrary::diagnosticText(diagnostics));
            return;
        }
        reload();
        emit schemeRemoved(id);
        emit catalogChanged();
        dialog->accept();
    });
    connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    dialog->open();
}
} // namespace ketplus

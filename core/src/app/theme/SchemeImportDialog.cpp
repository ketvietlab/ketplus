#include "KetBadge.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetSelect.h"
#include "KetTextArea.h"
#include "KetToggle.h"
#include "app/theme/SchemeLibraryPanel.h"
#include "ui/theme/ThemeSelection.h"
#include <QLabel>
#include <QScrollArea>
#include <QTextEdit>
#include <QVBoxLayout>

namespace ketplus {
namespace {
QString dispositionLabel(ImportDisposition disposition) {
    switch (disposition) {
    case ImportDisposition::Mapped:
        return QObject::tr("Mapped");
    case ImportDisposition::Approximated:
        return QObject::tr("Approximated");
    case ImportDisposition::Skipped:
        return QObject::tr("Skipped");
    }
    return {};
}
} // namespace
void SchemeLibraryPanel::chooseVariants(const SchemeImportBatch& batch) {
    auto* dialog = new ket::Dialog(tr("Choose theme variants"), this);
    connect(this, &QObject::destroyed, dialog, &ket::Dialog::reject);
    dialog->setObjectName(QStringLiteral("schemeVariants"));
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(body);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setMinimumHeight(180);
    auto* options = new QWidget(scroll);
    auto* optionLayout = new QVBoxLayout(options);
    QList<ket::Checkbox*> choices;
    for (const auto& candidate : batch.candidates) {
        auto* check = new ket::Checkbox(candidate.scheme.name, options);
        check->setChecked(true);
        choices.append(check);
        optionLayout->addWidget(check);
    }
    optionLayout->addStretch();
    scroll->setWidget(options);
    layout->addWidget(scroll);
    dialog->setBody(body);
    auto* cancel = dialog->addButton(tr("Cancel"));
    auto* next = dialog->addButton(tr("Review import"), ket::ButtonVariant::Primary);
    connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    for (auto* check : choices)
        connect(check, &QAbstractButton::toggled, dialog, [choices, next] {
            bool any = false;
            for (auto* option : choices)
                any |= option->isChecked();
            next->setEnabled(any);
        });
    connect(next, &QAbstractButton::clicked, dialog, &ket::Dialog::accept);
    connect(dialog, &ket::Dialog::accepted, this, [this, batch, choices] {
        QList<SchemeImportCandidate> selected;
        for (qsizetype i = 0; i < choices.size(); ++i)
            if (choices[i]->isChecked())
                selected.append(batch.candidates[i]);
        // Defer until the variant overlay has closed and restored focus.
        QMetaObject::invokeMethod(
            this, [this, selected] { reviewCandidates(selected); }, Qt::QueuedConnection);
    });
    connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    dialog->open();
}
void SchemeLibraryPanel::reviewCandidates(const QList<SchemeImportCandidate>& candidates) {
    if (candidates.isEmpty())
        return;
    auto* dialog = new ket::Dialog(tr("Review color scheme import"), this);
    connect(this, &QObject::destroyed, dialog, &ket::Dialog::reject);
    dialog->setObjectName(QStringLiteral("schemeImportReport"));
    dialog->setCardWidth(640);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    qsizetype mapped = 0, approximated = 0, skipped = 0;
    QStringList details;
    for (const auto& candidate : candidates) {
        details.append(candidate.scheme.name);
        for (const auto& item : candidate.report.items) {
            if (item.disposition == ImportDisposition::Mapped)
                ++mapped;
            else if (item.disposition == ImportDisposition::Approximated)
                ++approximated;
            else
                ++skipped;
            details.append(
                dispositionLabel(item.disposition) + QStringLiteral(" · ") + item.sourcePath +
                (item.targetPath.isEmpty() ? QString{} : QStringLiteral(" → ") + item.targetPath) +
                QStringLiteral("\n") + item.reason);
        }
        for (const auto& diagnostic : candidate.report.diagnostics)
            details.append(diagnostic.path + QStringLiteral(": ") + diagnostic.message);
        details.append(QString{});
    }
    auto* summary = new QLabel(tr("%1 scheme(s) · %2 mapped · %3 approximated · %4 skipped")
                                   .arg(candidates.size())
                                   .arg(mapped)
                                   .arg(approximated)
                                   .arg(skipped),
                               body);
    summary->setObjectName(QStringLiteral("schemeImportCounts"));
    summary->setTextFormat(Qt::PlainText);
    summary->setWordWrap(true);
    layout->addWidget(summary);
    auto* report = new ket::TextArea(body);
    report->setObjectName(QStringLiteral("schemeImportDetails"));
    report->setAccessibleName(tr("Import report details"));
    report->setMinimumLines(8);
    report->setMaximumLines(12);
    report->setText(details.join(u'\n'));
    report->editor()->setReadOnly(true);
    layout->addWidget(report);
    if (previewAvailable_) {
        auto* preview = new ket::Select(body);
        preview->setAccessibleName(tr("Preview variant"));
        for (const auto& candidate : candidates)
            preview->addItem(candidate.scheme.name);
        layout->addWidget(preview);
        auto* show = new ket::Button(tr("Preview on app"), ket::ButtonVariant::Secondary, body);
        layout->addWidget(show);
        connect(show, &QAbstractButton::clicked, this, [this, candidates, preview] {
            emit previewRequested(candidates[preview->currentIndex()].scheme);
        });
    }
    dialog->setBody(body);
    auto* cancel = dialog->addButton(tr("Cancel"));
    auto* accept = dialog->addButton(tr("Accept and import"), ket::ButtonVariant::Primary);
    accept->setObjectName(QStringLiteral("acceptSchemeImport"));
    connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    connect(accept, &QAbstractButton::clicked, this, [this, candidates, dialog] {
        QStringList added;
        for (const auto& candidate : candidates) {
            const auto result = library_.importCandidate(candidate);
            if (!result.success) {
                QString message = SchemeLibrary::diagnosticText(result.diagnostics);
                for (const auto& id : added) {
                    QList<SchemeDiagnostic> rollback;
                    if (!library_.catalog().remove(id, &rollback))
                        message += tr("\nCould not undo a partial import: ") +
                                   SchemeLibrary::diagnosticText(rollback);
                }
                dialog->reject();
                reload();
                emit catalogChanged();
                showError(message);
                return;
            }
            added.append(result.scheme.id);
        }
        const bool selected = selectScheme(added.first());
        if (selected)
            selectedId_ = added.first();
        reload();
        emit catalogChanged();
        dialog->accept();
        if (!selected)
            showError(tr("The schemes were imported, but could not be selected: ") +
                      selection_->lastError());
    });
    connect(dialog, &ket::Dialog::finished, this, [this] { emit previewFinished(); });
    connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    dialog->open();
}
} // namespace ketplus

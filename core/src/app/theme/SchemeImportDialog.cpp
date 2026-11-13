#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetBanner.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetSelect.h"
#include "KetTextArea.h"
#include "KetToggle.h"
#include "app/theme/SchemeLibraryPanel.h"
#include "ui/Theme.h"
#include <QEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QScrollArea>
#include <QShortcut>
#include <QTextEdit>
#include <QVBoxLayout>

namespace ketplus {
namespace {
// A local preview exposes the app instead of leaving a cached native-window
// snapshot under the report. The report continues to own both acceptance and cancellation.
class ImportPreviewNotice final : public QObject {
  public:
    explicit ImportPreviewNotice(ket::Dialog* dialog)
        : QObject(dialog), dialog_(dialog),
          banner_(new ket::Banner(
              ket::BannerTone::Info, {},
              tr("Preview only. Return to the report to accept or cancel the import."),
              dialog->parentWidget())) {
        banner_->setObjectName(QStringLiteral("schemePreviewBanner"));
        auto* back = banner_->setAction(tr("Return to report"));
        connect(back, &QAbstractButton::clicked, this, [this] {
            banner_->hide();
            escape_->setEnabled(false);
            dialog_->setProperty("importPreviewActive", false);
            dialog_->open();
        });
        escape_ = new QShortcut(QKeySequence(Qt::Key_Escape), banner_);
        escape_->setContext(Qt::WindowShortcut);
        escape_->setEnabled(false);
        connect(escape_, &QShortcut::activated, this, [this] { cancel(); });
        back->installEventFilter(this);
        dialog->parentWidget()->installEventFilter(this);
        connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this,
                [this] { place(); });
        banner_->hide();
    }
    ~ImportPreviewNotice() override { delete banner_.data(); }
    void show(const QString& name) {
        dialog_->setProperty("importPreviewActive", true);
        dialog_->hide();
        banner_->setTitle(tr("Preview: %1").arg(name));
        place();
        banner_->show();
        banner_->raise();
        banner_->action()->setFocus();
        escape_->setEnabled(true);
    }

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (banner_ && watched == banner_->action() && event->type() == QEvent::KeyPress &&
            static_cast<QKeyEvent*>(event)->key() == Qt::Key_Escape) {
            cancel();
            return true;
        }
        if (dialog_ && watched == dialog_->parentWidget() && event->type() == QEvent::Resize)
            place();
        return QObject::eventFilter(watched, event);
    }

  private:
    void cancel() {
        banner_->hide();
        escape_->setEnabled(false);
        dialog_->setProperty("importPreviewActive", false);
        dialog_->show();
        dialog_->reject();
    }
    void place() {
        if (!dialog_ || !banner_ || !dialog_->parentWidget())
            return;
        const int inset = qRound(ket::Appearance::instance().scaled(16));
        auto* window = dialog_->parentWidget();
        const int width = qMax(0, window->width() - 2 * inset);
        const int height = banner_->hasHeightForWidth() ? banner_->heightForWidth(width)
                                                        : banner_->sizeHint().height();
        banner_->setGeometry(inset, qMax(inset, window->height() - height - inset), width, height);
    }
    QPointer<ket::Dialog> dialog_;
    QPointer<ket::Banner> banner_;
    QShortcut* escape_;
};
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
    connect(this, &QObject::destroyed, dialog, [dialog] {
        if (dialog->property("importPreviewActive").toBool())
            dialog->show();
        dialog->reject();
    });
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
    connect(this, &QObject::destroyed, dialog, [dialog] {
        if (dialog->property("importPreviewActive").toBool())
            dialog->show();
        dialog->reject();
    });
    dialog->setObjectName(QStringLiteral("schemeImportReport"));
    dialog->setCardWidth(640);
    auto* body = new QWidget;
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    qsizetype mapped = 0, approximated = 0, skipped = 0;
    QStringList details;
    for (const auto& candidate : candidates) {
        details.append(candidate.scheme.name + u'\n');
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
                QStringLiteral("\n") + item.reason + u'\n');
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
    if (theme_) {
        auto* preview = new ket::Select(body);
        preview->setAccessibleName(tr("Preview variant"));
        for (const auto& candidate : candidates)
            preview->addItem(candidate.scheme.name);
        layout->addWidget(preview);
        auto* show = new ket::Button(tr("Preview on app"), ket::ButtonVariant::Secondary, body);
        show->setObjectName(QStringLiteral("previewImportedScheme"));
        auto* notice = new ImportPreviewNotice(dialog);
        layout->addWidget(show);
        connect(show, &QAbstractButton::clicked, this, [this, candidates, preview, notice] {
            const auto& scheme = candidates[preview->currentIndex()].scheme;
            emit previewRequested(scheme);
            notice->show(scheme.name);
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
                      theme_->schemeError());
    });
    connect(dialog, &ket::Dialog::finished, this, [this] { emit previewFinished(); });
    connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    dialog->open();
}
} // namespace ketplus

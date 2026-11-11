#include "KetBanner.h"
#include "KetButton.h"
#include "KetTextField.h"
#include "KetToast.h"
#include "git/GitChangesPanel.h"
#include <QVBoxLayout>
namespace ketplus {
void GitChangesPanel::createCommitControls(QVBoxLayout* layout) {
    auto* controls = new QWidget(this);
    auto* column = new QVBoxLayout(controls);
    column->setContentsMargins(12, 0, 12, 12);
    commitMessage_ = new ket::TextField(controls);
    commitMessage_->setObjectName(QStringLiteral("gitCommitMessage"));
    commitMessage_->setAccessibleName(tr("Commit message"));
    commitMessage_->setPlaceholderText(tr("Message for staged changes"));
    commitButton_ = new ket::Button(tr("Commit Staged"), ket::ButtonVariant::Primary, controls);
    commitButton_->setObjectName(QStringLiteral("gitCommitButton"));
    column->addWidget(commitMessage_);
    column->addWidget(commitButton_);
    mutationNotice_ =
        new ket::Banner(ket::BannerTone::Danger, tr("Git action failed"), {}, controls);
    mutationNotice_->hide();
    column->addWidget(mutationNotice_);
    layout->addWidget(controls);
    connect(commitMessage_, &QLineEdit::textChanged, this, &GitChangesPanel::updateCommitEnabled);
    connect(commitButton_, &QAbstractButton::clicked, this,
            [this] { emit commitRequested(commitMessage_->text()); });
    connect(commitMessage_, &QLineEdit::returnPressed, commitButton_, &QAbstractButton::click);
    updateCommitEnabled();
}
void GitChangesPanel::updateCommitEnabled() {
    if (!commitMessage_)
        return;
    commitButton_->setEnabled(!mutationRunning_ && hasStagedChanges_ &&
                              !commitMessage_->text().trimmed().isEmpty());
    commitMessage_->setEnabled(!mutationRunning_);
}
void GitChangesPanel::setMutationRunning(bool running) {
    mutationRunning_ = running;
    if (running && mutationNotice_)
        mutationNotice_->hide();
    updateCommitEnabled();
}
void GitChangesPanel::mutationFinished(const QString& operation, bool ok, const QString& error) {
    mutationNotice_->setMessage(error);
    mutationNotice_->setVisible(!ok);
    if (ok && operation == QStringLiteral("commit")) {
        commitMessage_->clear();
        ket::Toast::showMessage(this, tr("Staged changes committed"));
    }
}
} // namespace ketplus

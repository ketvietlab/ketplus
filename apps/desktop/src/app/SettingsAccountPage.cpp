#include "app/SettingsAccountPage.h"
#include "KetAvatar.h"
#include "KetButton.h"
#include "KetIcons.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {
namespace {

QLabel* textLabel(const QString& text, const QString& objectName, QWidget* parent) {
    auto* value = new QLabel(text, parent);
    value->setObjectName(objectName);
    value->setTextFormat(Qt::PlainText);
    value->setWordWrap(true);
    return value;
}

} // namespace

SettingsAccountPage::SettingsAccountPage(StatusReader reader, QWidget* parent)
    : QWidget(parent), reader_(std::move(reader)) {
    setObjectName(QStringLiteral("settingsWorkspaceAccount"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 24);
    layout->setSpacing(0);
    layout->addWidget(
        textLabel(tr("Account & devices"), QStringLiteral("settingsWorkspaceSectionTitle"), this));
    layout->addSpacing(14);
    layout->addWidget(textLabel(tr("Your KetPlus account, and the phones and Linux machines "
                                   "that use this Mac."),
                                QStringLiteral("settingsWorkspaceIntro"), this));
    layout->addSpacing(18); // the mock's column spacing

    auto* identity = new QHBoxLayout;
    identity->setSpacing(12);
    avatar_ = new ket::Avatar({}, this);
    avatar_->setObjectName(QStringLiteral("accountAvatar"));
    avatar_->setDiameter(40);
    identity->addWidget(avatar_, 0, Qt::AlignVCenter);
    auto* copy = new QVBoxLayout;
    copy->setSpacing(1);
    name_ = textLabel({}, QStringLiteral("settingsWorkspaceSubheading"), this);
    name_->setProperty("accountRole", QStringLiteral("name"));
    meta_ = textLabel({}, QStringLiteral("settingsWorkspaceHelp"), this);
    meta_->setProperty("accountRole", QStringLiteral("meta"));
    copy->addWidget(name_);
    copy->addWidget(meta_);
    identity->addLayout(copy, 1);
    layout->addLayout(identity);
    layout->addSpacing(18);

    auto* actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto* manage = new ket::Button(tr("Manage connections"), ket::ButtonVariant::Secondary, this);
    manage->setObjectName(QStringLiteral("accountManageConnections"));
    manage->setIcon(ket::Icons::icon(u"network"));
    manage_ = manage;
    auto* signOut = new ket::Button(tr("Sign out"), ket::ButtonVariant::Ghost, this);
    signOut->setObjectName(QStringLiteral("accountSignOut"));
    signOut->setIcon(ket::Icons::icon(u"log-out"));
    signOut_ = signOut;
    actions->addWidget(manage_);
    actions->addWidget(signOut_);
    actions->addStretch(1);
    layout->addLayout(actions);
    layout->addStretch(1);

    connect(manage_, &QAbstractButton::clicked, this, &SettingsAccountPage::manageRequested);
    connect(signOut_, &QAbstractButton::clicked, this, &SettingsAccountPage::signOutRequested);
    // Until the daemon answers, the page claims nothing about the account.
    avatar_->hide();
    name_->setText(tr("Checking your account…"));
    meta_->hide();
    signOut_->hide();
}

void SettingsAccountPage::setStatusReader(StatusReader reader) {
    reader_ = std::move(reader);
    if (isVisible())
        refresh();
}

void SettingsAccountPage::refresh() {
    if (!reader_)
        return;
    reader_([page = QPointer<SettingsAccountPage>(this)](const QJsonObject& status) {
        if (page)
            page->apply(status);
    });
}

void SettingsAccountPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    refresh(); // Connections may have signed in or out since the page was last shown
}

void SettingsAccountPage::apply(const QJsonObject& status) {
    const bool signedIn = status.value(QStringLiteral("configured")).toBool();
    const QJsonObject account = status.value(QStringLiteral("account")).toObject();
    const QString displayName = account.value(QStringLiteral("display_name")).toString();
    const QString username = account.value(QStringLiteral("username")).toString();
    if (!signedIn) {
        name_->setText(tr("Not signed in"));
        meta_->setText(tr("Sign in to use this Mac from your phone or a Linux machine."));
    } else {
        // Enrollments from before ADR 0109 have no name until the next sign-in.
        name_->setText(!displayName.isEmpty() ? displayName
                                              : (!username.isEmpty() ? username : tr("Signed in")));
        meta_->setText(!username.isEmpty() && username != name_->text()
                           ? tr("%1 · ketPlus Pro").arg(username)
                           : tr("ketPlus Pro"));
    }
    meta_->show();
    avatar_->setName(signedIn ? name_->text() : QString());
    avatar_->setVisible(signedIn);
    manage_->setText(signedIn ? tr("Manage connections") : tr("Sign in…"));
    signOut_->setVisible(signedIn);
}

} // namespace ketplus

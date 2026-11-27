#include "ui/AboutDialog.h"

#include "KetButton.h"
#include "KetDialog.h"

#include <QCoreApplication>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>

namespace ketplus {

void showAboutDialog(QWidget* parent, const BrandLockup::Edition edition) {
    Q_ASSERT(parent);
    if (!parent)
        return;
    if (auto* open = parent->findChild<ket::Dialog*>(QStringLiteral("aboutDialog"))) {
        open->raise();
        open->activateWindow();
        return;
    }
    auto* lockup = new BrandLockup(edition);
    lockup->setWordSize(26);
    auto* dialog = new ket::Dialog(QObject::tr("About %1").arg(lockup->productName()), parent);
    dialog->setObjectName(QStringLiteral("aboutDialog"));
    dialog->setAttribute(Qt::WA_DeleteOnClose);

    auto* body = new QWidget(dialog);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    lockup->setParent(body);
    layout->addWidget(lockup, 0, Qt::AlignLeft);
    auto* version =
        new QLabel(QObject::tr("Version %1").arg(QCoreApplication::applicationVersion()), body);
    version->setObjectName(QStringLiteral("aboutVersion"));
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    version->setVisible(!QCoreApplication::applicationVersion().isEmpty());
    layout->addWidget(version);
    dialog->setBody(body);

    auto* close = dialog->addButton(QObject::tr("Close"), ket::ButtonVariant::Primary);
    QObject::connect(close, &QAbstractButton::clicked, dialog, &ket::Dialog::accept);
    dialog->open();
}

} // namespace ketplus

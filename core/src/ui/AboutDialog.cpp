#include "ui/AboutDialog.h"

#include "AboutSourceIdentity.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetProfile.h"
#include "KetTokens.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QVBoxLayout>

namespace ketplus {
namespace {
class AboutBody final : public QScrollArea {
  public:
    using QScrollArea::QScrollArea;
    QSize sizeHint() const override { return widget() ? widget()->sizeHint() : QSize(); }
    QSize minimumSizeHint() const override { return {0, 0}; }
};
} // namespace

AboutInformation aboutInformation(const BrandLockup::Edition edition,
                                  const QString& applicationDirectory, const QString& revision) {
    AboutInformation information;
    information.revision = revision;
    const QRegularExpression identity(QStringLiteral("^([0-9a-f]{40}|[0-9a-f]{64})(-dirty)?$"));
    const auto match = identity.match(revision);
    if (match.hasMatch())
        information.source =
            QUrl(QStringLiteral("https://github.com/ketvietlab/ketplus/tree/") + match.captured(1));
    const QDir application(applicationDirectory);
    const QString product = edition == BrandLockup::Edition::Pro ? QStringLiteral("ketplus")
                                                                 : QStringLiteral("ketplus-cm");
    const QStringList layouts{QStringLiteral("../Resources/licenses"),
                              QStringLiteral("../share/licenses/") + product,
                              QStringLiteral("licenses")};
    for (const auto& layout : layouts) {
        const QDir directory(application.absoluteFilePath(layout));
        const QFileInfo license(directory.filePath(QStringLiteral("LICENSE")));
        const QFileInfo notices(directory.filePath(QStringLiteral("THIRD_PARTY_NOTICES.md")));
        if (license.isFile() && license.isReadable())
            information.licenses = QUrl::fromLocalFile(directory.absolutePath());
        if (notices.isFile() && notices.isReadable())
            information.notices = QUrl::fromLocalFile(notices.absoluteFilePath());
        if (!information.licenses.isEmpty() || !information.notices.isEmpty())
            break;
    }
    return information;
}

void showAboutDialog(QWidget* parent, const BrandLockup::Edition edition) {
    showAboutDialog(parent, edition,
                    aboutInformation(edition, QCoreApplication::applicationDirPath(),
                                     QStringLiteral(KETPLUS_ABOUT_SOURCE_REVISION)));
}

void showAboutDialog(QWidget* parent, const BrandLockup::Edition edition,
                     const AboutInformation& information) {
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
    auto* actions = new QHBoxLayout;
    actions->setContentsMargins(0, 0, 0, 0);
    layout->setContentsMargins(0, 0, 0, 0);
    auto updateSpacing = [layout, actions] {
        layout->setSpacing(qRound(ket::Appearance::instance().scaled(ket::Tokens::px(u"space-2"))));
        actions->setSpacing(layout->spacing());
    };
    updateSpacing();
    QObject::connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, body,
                     updateSpacing);
    lockup->setParent(body);
    layout->addWidget(lockup, 0, Qt::AlignLeft);
    auto* version =
        new QLabel(QObject::tr("Version %1").arg(QCoreApplication::applicationVersion()), body);
    version->setObjectName(QStringLiteral("aboutVersion"));
    version->setTextInteractionFlags(Qt::TextSelectableByMouse);
    version->setVisible(!QCoreApplication::applicationVersion().isEmpty());
    layout->addWidget(version);
    auto* revision = new QLabel(information.source.isEmpty()
                                    ? QObject::tr("Source revision unavailable")
                                    : QObject::tr("Source revision: %1").arg(information.revision),
                                body);
    revision->setObjectName(QStringLiteral("aboutRevision"));
    revision->setTextFormat(Qt::PlainText);
    revision->setWordWrap(true);
    revision->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(revision);
    auto* status = new QLabel(body);
    status->setObjectName(QStringLiteral("aboutAccessStatus"));
    status->setWordWrap(true);
    status->setTextFormat(Qt::PlainText);
    if (information.notices.isEmpty() || information.licenses.isEmpty())
        status->setText(
            QObject::tr("Some license or notice files are unavailable in this installation."));
    if (information.revision.endsWith(QStringLiteral("-dirty")))
        status->setText(status->text() +
                        (status->text().isEmpty() ? QString() : QStringLiteral("\n")) +
                        QObject::tr("Uncommitted changes are not included in the linked source."));
    auto addAccess = [body, actions, status](const QString& text, const QString& name,
                                             const QUrl& url) {
        auto* button = new ket::Button(text, ket::ButtonVariant::Secondary, body);
        button->setObjectName(name);
        button->setEnabled(!url.isEmpty());
        button->setAccessibleName(text);
        auto updateHeight = [button] { button->setMinimumHeight(button->sizeHint().height()); };
        updateHeight();
        QObject::connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, button,
                         updateHeight);
        QObject::connect(button, &QAbstractButton::clicked, body, [status, url] {
            if (!QDesktopServices::openUrl(url))
                status->setText(
                    QObject::tr("Could not open the selected source or notice location."));
        });
        actions->addWidget(button);
    };
    addAccess(QObject::tr("View public source"), QStringLiteral("aboutSource"), information.source);
    addAccess(QObject::tr("Third-party notices"), QStringLiteral("aboutNotices"),
              information.notices);
    addAccess(QObject::tr("License files"), QStringLiteral("aboutLicenses"), information.licenses);
    layout->addLayout(actions);
    layout->addWidget(status);
    auto updateText = [version, revision, status] {
        const auto& appearance = ket::Appearance::instance();
        for (auto* label : {version, revision, status}) {
            auto palette = label->palette();
            palette.setColor(QPalette::WindowText,
                             ket::Tokens::color(u"text-main", appearance.theme()));
            label->setPalette(palette);
            auto font = label->font();
            const auto family = ket::Tokens::availableFamily(u"font-sans");
            if (!family.isEmpty())
                font.setFamily(family);
            font.setPixelSize(qRound(appearance.scaled(ket::Profile::current().buttonFontPixels)));
            font.setWeight(QFont::Normal);
            label->setFont(font);
        }
    };
    updateText();
    QObject::connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, body,
                     updateText);
    QObject::connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, body, updateText);
    auto* scroll = new AboutBody(dialog);
    scroll->setObjectName(QStringLiteral("aboutScrollArea"));
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setSizeAdjustPolicy(QAbstractScrollArea::AdjustToContents);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(body);
    body->setAutoFillBackground(false);
    scroll->viewport()->setAutoFillBackground(false);
    dialog->setBody(scroll);
    QObject::connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, scroll,
                     [scroll] { scroll->updateGeometry(); });

    auto* close = dialog->addButton(QObject::tr("Close"), ket::ButtonVariant::Primary);
    QObject::connect(close, &QAbstractButton::clicked, dialog, &ket::Dialog::accept);
    dialog->open();
}

} // namespace ketplus

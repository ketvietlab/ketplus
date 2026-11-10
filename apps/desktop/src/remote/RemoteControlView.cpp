#include "remote/RemoteControlView.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetIcons.h"
#include "KetProfile.h"
#include "KetRecipes.h"
#include "KetTextField.h"
#include "KetTokens.h"
#include "ui/ChromeGeometry.h"
#include "ui/SymbolIcon.h"

#include "DaemonClient.h"
#include "remote/LinuxMachinesSection.h"
#include "remote/PairingQrCode.h"
#include "remote/RemoteActivitySection.h"
#include "remote/RemoteConnectionsIntro.h"
#include "ui/TypeScale.h"

#include <QApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionButton>
#include <QTimeZone>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {
namespace {

// Semantic Két Symbol names used by Connections.
constexpr auto monitorIcon = "monitor";
constexpr auto phoneIcon = "smartphone";
constexpr auto shieldIcon = "shield";
constexpr auto unplugIcon = "unplug";
constexpr auto plusIcon = "plus";
constexpr auto logOutIcon = "log-out";

QIcon symbolIcon(const QByteArray& body, const QColor& color, const int size = 16) {
    return QIcon(symbol_icon::pixmap(body, color, QSize(size, size)));
}

class SymbolIconLabel final : public QLabel {
  public:
    SymbolIconLabel(QByteArray body, int size, QWidget* parent = nullptr)
        : QLabel(parent), body_(std::move(body)) {
        setFixedSize(size, size);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        symbol_icon::paint(painter, body_, palette().color(QPalette::Highlight), rect());
    }

  private:
    QByteArray body_;
};

QLabel* label(const QString& text, const char* role, QWidget* parent = nullptr) {
    auto* value = new QLabel(text, parent);
    value->setProperty("remoteRole", role);
    value->setWordWrap(true);
    return value;
}

QLabel* badge(const QString& text, const QString& tone = QStringLiteral("neutral")) {
    auto* value = label(text, "badge");
    value->setProperty("remoteTone", tone);
    value->setAlignment(Qt::AlignCenter);
    value->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return value;
}

enum class DialogChoice { Cancel, Confirm };

DialogChoice runDecisionDialog(QWidget* parent, const QString& objectName, const QString& title,
                               const QString& body, const QString& help, const QString& cancelText,
                               const QString& confirmText, const bool danger,
                               const QByteArray& confirmIcon = {}) {
    QDialog dialog(parent);
    dialog.setObjectName(objectName);
    dialog.setProperty("remoteRole", QStringLiteral("modal"));
    dialog.setWindowTitle(title);
    dialog.setWindowModality(Qt::WindowModal);
    dialog.setFixedWidth(500);
    auto* root = new QVBoxLayout(&dialog);
    chrome_geometry::margins(root, 0, 0, 0, 0);
    chrome_geometry::spacing(root, 0);

    auto* header = new QFrame(&dialog);
    header->setProperty("remoteRole", QStringLiteral("modalHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    chrome_geometry::margins(headerLayout, 22, 14, 14, 14);
    chrome_geometry::spacing(headerLayout, 12);
    headerLayout->addWidget(label(title, "modalTitle"), 1);
    auto* close = new ket::IconButton(ket::Icons::icon(u"close"), QStringLiteral("Close"), header);
    close->setProperty("remoteTertiary", true);
    close->setAccessibleName(QStringLiteral("Close"));
    chrome_geometry::fixedSize(close, 30, 30);
    QObject::connect(close, &QAbstractButton::clicked, &dialog, &QDialog::reject);
    headerLayout->addWidget(close);
    root->addWidget(header);

    auto* content = new QWidget(&dialog);
    content->setProperty("remoteRole", QStringLiteral("modalBody"));
    auto* contentLayout = new QVBoxLayout(content);
    chrome_geometry::margins(contentLayout, 22, 21, 22, 22);
    chrome_geometry::spacing(contentLayout, 9);
    contentLayout->addWidget(label(body, "modalBodyText"));
    if (!help.isEmpty())
        contentLayout->addWidget(label(help, "description"));
    root->addWidget(content);

    auto* footer = new QFrame(&dialog);
    footer->setProperty("remoteRole", QStringLiteral("modalFooter"));
    auto* actions = new QHBoxLayout(footer);
    chrome_geometry::margins(actions, 22, 13, 22, 13);
    chrome_geometry::spacing(actions, 8);
    actions->addStretch();
    auto* cancel = new ket::Button(cancelText, ket::ButtonVariant::Secondary, footer);
    cancel->setObjectName(objectName + QStringLiteral("Cancel"));
    QObject::connect(cancel, &QAbstractButton::clicked, &dialog, &QDialog::reject);
    actions->addWidget(cancel);
    auto* confirm = new ket::Button(
        confirmText, danger ? ket::ButtonVariant::Danger : ket::ButtonVariant::Primary, footer);
    confirm->setObjectName(objectName + QStringLiteral("Confirm"));
    confirm->setProperty(danger ? "remoteDanger" : "remotePrimary", true);
    if (!confirmIcon.isEmpty()) {
        confirm->setIcon(symbolIcon(
            confirmIcon,
            danger ? ket::Tokens::color(u"danger", ket::Appearance::instance().theme())
                   : ket::Tokens::color(u"action-primary-ink", ket::Appearance::instance().theme()),
            14));
        chrome_geometry::iconSize(confirm, QSize(14, 14));
    }
    QObject::connect(confirm, &QAbstractButton::clicked, &dialog, &QDialog::accept);
    actions->addWidget(confirm);
    root->addWidget(footer);
    return dialog.exec() == QDialog::Accepted ? DialogChoice::Confirm : DialogChoice::Cancel;
}

// Native Qt mapping of @ketvietlab/design-system's public semantic contract.
// Keep these roles aligned with packages/design-system/src/foundations/tokens.css;
// the values below were verified against ketviet e8adee2d (design system 0.1.44).
struct KdsPalette final {
    QString page, panel, raised, hover, text, secondary, muted, disabled, border, strong, input;
    QString accent, accentHover, accentActive, accentSubtle, accentBorder, accentText;
    QString primary, primaryHover, primaryActive;
    QString positive, positiveBg, positiveBorder, warning, warningBg, warningBorder;
    QString danger, dangerBg, dangerBorder;
};

KdsPalette kdsPalette(const bool dark) {
    const auto theme = dark ? ket::Theme::Dark : ket::Theme::Light;
    const auto color = [theme](QStringView name) {
        return ket::Tokens::color(name, theme).name(QColor::HexArgb);
    };
    return {color(u"page-bg"),
            color(u"panel-bg"),
            color(u"surface-raised"),
            color(u"surface-hover"),
            color(u"text-main"),
            color(u"text-secondary"),
            color(u"text-muted"),
            color(u"text-disabled"),
            color(u"panel-border"),
            color(u"border-strong"),
            color(u"input-bg"),
            color(u"accent"),
            color(u"accent-hover"),
            color(u"accent-active"),
            color(u"accent-subtle"),
            color(u"accent-border"),
            color(u"accent-text"),
            color(u"action-primary-bg"),
            color(u"action-primary-hover"),
            color(u"action-primary-active"),
            color(u"positive"),
            color(u"positive-bg"),
            color(u"positive-border"),
            color(u"warning"),
            color(u"warning-bg"),
            color(u"warning-border"),
            color(u"danger"),
            color(u"danger-bg"),
            color(u"danger-border")};
}

QString remoteStyle(const KdsPalette& p, const int zoomPercent) {
    QString css = TypeScale::apply(QStringLiteral(R"QSS(
QWidget#remoteControlWorkspace, QWidget[remoteRole="page"], QScrollArea[remoteRole="viewport"],
QScrollArea[remoteRole="viewport"] > QWidget > QWidget { background: @page@; }
QWidget[remoteRole="dialogPage"] { background:@panel@; }
QFrame[remoteRole="contextBar"] { min-height:34px; max-height:34px; border:0; border-bottom:1px solid @border@; background:@panel@; }
QLabel[remoteRole="context"] { color:@muted@; font-size:{{caption}}px; font-weight:600; }
QFrame[remoteRole="pageHeader"] { border:0; background:@page@; }
QLabel[remoteRole="title"] { color:@text@; font-size:{{heading}}px; font-weight:600; }
QLabel[remoteRole="connectionsTitle"] { color:@text@; font-size:{{px:30}}px; font-weight:600; }
QLabel[remoteRole="eyebrow"] { color:@accentText@; font-size:{{micro}}px; font-weight:600; letter-spacing:1px; }
QLabel[remoteRole="heroTitle"] { color:@text@; font-size:{{px:40}}px; font-weight:600; }
QLabel[remoteRole="heroCopy"] { color:@muted@; font-size:{{px:14}}px; }
QLabel[remoteRole="description"] { color:@muted@; font-size:{{body}}px; }
QLabel[remoteRole="surfaceTitle"] { color:@text@; font-size:{{title}}px; font-weight:600; }
QLabel[remoteRole="dialogTitle"] { color:@text@; font-size:{{heading}}px; font-weight:600; }
QLabel[remoteRole="pairTitle"] { color:@text@; font-size:{{title}}px; font-weight:600; }
QLabel[remoteRole="welcomeTitle"] { color:@text@; font-size:{{display}}px; font-weight:620; }
QLabel[remoteRole="welcomeText"] { color:@text@; font-size:{{reading}}px; }
QLabel[remoteRole="introTitle"] { color:@text@; font-size:{{px:22}}px; font-weight:500; }
QLabel[remoteRole="introState"] { color:@muted@; font-size:{{body}}px; }
QLabel[remoteRole="introNote"] { color:@secondary@; font-size:{{body}}px; }
QLabel[remoteRole="introIcon"] { color:@muted@; }
QLabel[remoteRole="introSection"] { color:@text@; font-size:{{title}}px; font-weight:600; }
QLabel[remoteRole="introStepTitle"] { color:@text@; font-size:{{body}}px; font-weight:600; }
QLabel[remoteRole="introStepNumber"] { color:@muted@; font-family:"@mono@"; font-size:{{body}}px; }
QFrame[remoteRole="introStepBar"] { border:0; background:@accent@; }
QLabel[remoteRole="overviewTitle"] { color:@text@; font-size:{{heading}}px; font-weight:600; }
QLabel#linuxMachinesNotice { color:@secondary@; font-size:{{body}}px; }
QLabel#linuxMachinesNotice[remoteTone="danger"] { color:@danger@; }
QFrame[remoteRole="surface"] { border:1px solid @border@; border-radius:7px; background:@panel@; }
QFrame[remoteRole="surfaceAccent"] { border:1px solid @accentBorder@; border-radius:7px; background:@panel@; }
QFrame[remoteRole="signInCard"] { border:1px solid @border@; border-radius:9px; background:@panel@; }
QFrame[remoteRole="connectionOverview"] { border:0; border-bottom:1px solid @border@; background:transparent; }
QFrame[remoteRole="deviceIllustration"] { border:1px solid @accentBorder@; border-radius:10px; background:@accentSubtle@; }
QFrame[remoteRole="divider"] { border:0; background:@border@; }
QFrame[remoteRole="statusRow"] { border:0; background:transparent; }
QLabel[remoteRole="rowTitle"] { color:@secondary@; font-size:{{body}}px; font-weight:500; }
QLabel[remoteRole="rowDetail"] { color:@muted@; font-size:{{caption}}px; }
QLabel[remoteRole="statusValue"] { color:@secondary@; font-size:{{body}}px; font-weight:600; }
QLabel[remoteRole="statusValue"][remoteTone="positive"] { color:@positive@; }
QLabel[remoteRole="statusValue"][remoteTone="warning"] { color:@warning@; }
QLabel[remoteRole="statusValue"][remoteTone="danger"] { color:@danger@; }
QLabel[remoteRole="badge"] { min-height:20px; padding:0 8px; border:1px solid @border@; border-radius:10px; background:@raised@; color:@secondary@; font-size:{{micro}}px; font-weight:600; }
QLabel[remoteRole="badge"][remoteTone="positive"] { border-color:@positiveBorder@; background:@positiveBg@; color:@positive@; }
QLabel[remoteRole="badge"][remoteTone="warning"] { border-color:@warningBorder@; background:@warningBg@; color:@warning@; }
QLabel[remoteRole="badge"][remoteTone="danger"] { border-color:@dangerBorder@; background:@dangerBg@; color:@danger@; }
QFrame[remoteRole="activityList"] { border:1px solid @border@; border-radius:7px; background:@panel@; }
QScrollArea#remoteActivityDialogScroll QFrame[remoteRole="activityList"] { border-radius:7px; }
QFrame[remoteRole="activityRow"] { border:0; border-top:1px solid @border@; background:transparent; }
QFrame[remoteRole="activityRow"][remoteFirst="true"] { border-top:0; }
QFrame[remoteRole="activityRow"]:focus { border:2px solid @accent@; border-radius:5px; }
QLabel[remoteRole="activityIcon"] { color:@muted@; }
QLabel[remoteRole="activityTitle"] { color:@text@; font-size:{{body}}px; font-weight:500; }
QLabel[remoteRole="activityTime"], QLabel[remoteRole="activityDetail"] { color:@muted@; font-size:{{caption}}px; }
QFrame[remoteRole="routeNode"] { border:1px solid @border@; border-radius:5px; background:@raised@; }
QLabel[remoteRole="routeIcon"] { color:@accentText@; font-size:{{glyphSmall}}px; font-weight:600; }
QLabel[remoteRole="routeTitle"] { color:@text@; font-size:{{body}}px; font-weight:600; }
QLabel[remoteRole="routeDetail"], QLabel[remoteRole="routeArrow"] { color:@muted@; font-size:{{micro}}px; }
QLabel[remoteRole="emptyIcon"] { color:@accentText@; font-size:{{glyphLarge}}px; font-weight:500; }
QLabel[remoteRole="emptyTitle"] { color:@text@; font-size:{{title}}px; font-weight:600; }
QLabel[remoteRole="emptyText"] { color:@muted@; font-size:{{body}}px; }
QLabel[remoteRole="sectionTitle"] { color:@text@; font-size:{{reading}}px; font-weight:600; }
QLabel[remoteRole="sectionCount"] { color:@muted@; font-size:{{caption}}px; }
QLabel[remoteRole="tabActive"] { padding:8px 12px; border-radius:7px; background:@raised@; color:@text@; font-size:{{body}}px; font-weight:500; }
QFrame[remoteRole="devicePanel"] { border:1px solid @strong@; border-radius:12px; background:@panel@; }
QFrame[remoteRole="deviceRow"] { border:0; border-top:1px solid @border@; background:transparent; }
QLabel[remoteRole="deviceIcon"] { color:@text@; font-size:{{heading}}px; }
QFrame[remoteRole="deviceIconSurface"] { border:1px solid @border@; border-radius:7px; background:@raised@; }
QLabel[remoteRole="deviceName"] { color:@text@; font-size:{{reading}}px; font-weight:500; }
QLabel[remoteRole="deviceMeta"] { color:@muted@; font-size:{{caption}}px; }
QLabel[remoteRole="accountName"] { color:@text@; font-size:{{body}}px; font-weight:600; }
QLabel[remoteRole="accountMeta"] { color:@muted@; font-size:{{caption}}px; }
QLabel[remoteRole="trustNote"] { color:@muted@; font-size:{{caption}}px; }
QLabel[remoteRole="feature"] { color:@secondary@; font-size:{{body}}px; }
QDialog[remoteRole="modal"] { background:@panel@; }
QFrame[remoteRole="modalHeader"] { border:0; border-bottom:1px solid @border@; background:@panel@; }
QFrame[remoteRole="modalFooter"] { border:0; border-top:1px solid @border@; background:@raised@; }
QWidget[remoteRole="modalBody"] { background:@panel@; }
QLabel[remoteRole="modalTitle"] { color:@text@; font-size:{{title}}px; font-weight:600; }
QLabel[remoteRole="modalBodyText"] { color:@text@; font-size:{{reading}}px; line-height:1.5; }
QLabel[remoteRole="pairSteps"] { color:@secondary@; font-size:{{reading}}px; line-height:2; }
QLabel[remoteRole="pairCode"] { min-height:70px; padding:0 18px; border:1px solid @border@;
 border-radius:7px; background:@raised@; color:@text@; font-size:{{display}}px; font-weight:550; letter-spacing:7px; }
QLabel[remoteRole="pairFacts"] { padding:14px; border:1px solid @border@; border-radius:7px;
 background:@raised@; color:@secondary@; font-size:{{reading}}px; }
QFrame[remoteRole="notice"] { border:1px solid @accentBorder@; border-left:3px solid @accent@; border-radius:5px; background:@accentSubtle@; }
QFrame[remoteRole="notice"][remoteTone="warning"] { border-color:@warningBorder@; border-left-color:@warning@; background:@warningBg@; }
QLabel[remoteRole="noticeTitle"] { color:@text@; font-size:{{body}}px; font-weight:700; }
QLabel[remoteRole="noticeText"] { color:@secondary@; font-size:{{body}}px; }
QLabel[remoteRole="noticeText"][remoteFailure="true"] { color:@danger@; }
QLabel#remoteNotice, QLabel#remoteWelcomeNotice { min-height:36px; padding:0 12px; border:1px solid @accentBorder@;
 border-left:3px solid @accent@; border-radius:5px; background:@accentSubtle@; }
QLabel[remoteRole="fieldLabel"] { color:@secondary@; font-size:{{body}}px; font-weight:500; }
QLineEdit { min-height:32px; padding:0 10px; border:1px solid @strong@; border-radius:5px; background:@input@; color:@text@; font-size:{{body}}px; selection-background-color:@accent@; }
QLineEdit:hover { border-color:@muted@; } QLineEdit:focus { border-color:@accent@; }
QLineEdit:read-only { background:@raised@; color:@secondary@; }
QPushButton { min-height:32px; padding:0 12px; border:1px solid @strong@; border-radius:5px; background:@input@; color:@secondary@; font-size:{{body}}px; font-weight:500; }
QPushButton:hover { border-color:@muted@; background:@hover@; color:@text@; }
QPushButton:pressed { background:@raised@; }
QPushButton[remotePrimary="true"] { border-color:@primary@; background:@primary@; color:white; font-weight:600; }
QPushButton[remotePrimary="true"]:hover { border-color:@primaryHover@; background:@primaryHover@; color:white; }
QPushButton[remotePrimary="true"]:pressed { border-color:@primaryActive@; background:@primaryActive@; }
QPushButton[remoteTertiary="true"] { border-color:transparent; background:transparent; }
QPushButton[remoteDanger="true"] { border-color:@dangerBorder@; background:transparent; color:@danger@; }
QPushButton[remoteDanger="true"]:hover { border-color:@danger@; background:@dangerBg@; color:@danger@; }
QPushButton:disabled { border-color:@border@; background:@raised@; color:@disabled@; }
QLabel[remoteRole="qr"] { background:white; border:10px solid white; border-radius:5px; }
)QSS"),
                                   zoomPercent);
    const QList<QPair<QString, QString>> replacements = {{"@page@", p.page},
                                                         {"@panel@", p.panel},
                                                         {"@raised@", p.raised},
                                                         {"@hover@", p.hover},
                                                         {"@text@", p.text},
                                                         {"@secondary@", p.secondary},
                                                         {"@muted@", p.muted},
                                                         {"@disabled@", p.disabled},
                                                         {"@border@", p.border},
                                                         {"@strong@", p.strong},
                                                         {"@input@", p.input},
                                                         {"@accent@", p.accent},
                                                         {"@accentHover@", p.accentHover},
                                                         {"@accentActive@", p.accentActive},
                                                         {"@accentSubtle@", p.accentSubtle},
                                                         {"@accentBorder@", p.accentBorder},
                                                         {"@accentText@", p.accentText},
                                                         {"@primaryHover@", p.primaryHover},
                                                         {"@primaryActive@", p.primaryActive},
                                                         {"@primary@", p.primary},
                                                         {"@positive@", p.positive},
                                                         {"@positiveBg@", p.positiveBg},
                                                         {"@positiveBorder@", p.positiveBorder},
                                                         {"@warning@", p.warning},
                                                         {"@warningBg@", p.warningBg},
                                                         {"@warningBorder@", p.warningBorder},
                                                         {"@danger@", p.danger},
                                                         {"@dangerBg@", p.dangerBg},
                                                         {"@dangerBorder@", p.dangerBorder}};
    for (const auto& replacement : replacements)
        css.replace(replacement.first, replacement.second);
    css.replace(QStringLiteral("@mono@"), ket::Profile::current().monoFamily());
    return css;
}

} // namespace

RemoteControlView::RemoteControlView(QWidget* parent)
    : QWidget(parent), daemon_(new remote::DaemonClient(this)), refreshTimer_(new QTimer(this)),
      pages_(new QStackedWidget(this)) {
#if defined(KETPLUS_REMOTE_DEMO_AVAILABLE)
    demoMode_ = qEnvironmentVariableIntValue("KETPLUS_REMOTE_DEMO") == 1;
#endif
    setObjectName(QStringLiteral("remoteControlWorkspace"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto* root = new QVBoxLayout(this);
    chrome_geometry::margins(root, 0, 0, 0, 0);
    root->addWidget(pages_);
    pages_->addWidget(createControlPage());
    pages_->setCurrentIndex(0);
    // The mock's sign-in dialog: the card's own header and close, the action in its footer.
    signInDialog_ = new ket::Dialog(QStringLiteral("Sign in"), this);
    signInDialog_->setObjectName(QStringLiteral("remoteSignInDialog"));
    signInDialog_->setCardWidth(500);
    signInDialog_->setBody(createSignInPage());
    signInButton_ = signInDialog_->addButton({}, ket::ButtonVariant::Primary);
    signInButton_->setObjectName(QStringLiteral("remoteSignInButton"));
    signInButton_->setIcon(ket::Icons::icon(u"log-in"));
    resetSignInButton();
    connect(signInButton_, &QAbstractButton::clicked, this, &RemoteControlView::signIn);
    connect(signInDialog_, &ket::Dialog::rejected, this, &RemoteControlView::cancelSignIn);
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            &RemoteControlView::applyDesignSystemStyle);
    applyDesignSystemStyle();

    refreshTimer_->setInterval(2'000);
    connect(refreshTimer_, &QTimer::timeout, this, &RemoteControlView::refreshStatus);
    connect(daemon_, &remote::DaemonClient::readyChanged, this, [this](const bool ready) {
        if (ready)
            refreshStatus();
    });
    connect(daemon_, &remote::DaemonClient::disconnected, this,
            [this](const QString& reason) { setNotice(reason, true); });
    refreshTimer_->start();
}

RemoteControlView::~RemoteControlView() {
    delete signInDialog_.data();
    delete pairingDialog_.data();
}

void RemoteControlView::applyDesignSystemStyle() {
    if (applyingDesignSystemStyle_)
        return;
    applyingDesignSystemStyle_ = true;
    const int zoomPercent = InterfaceZoomMetrics::current();
    // The kit's theme, as the dialog cards read it: a palette under this view's own sheet
    // can lag a theme change and leave the page and its cards in different themes.
    const bool dark = ket::Appearance::instance().dark();
    const KdsPalette colors = kdsPalette(dark);
    const QString sheet = remoteStyle(colors, zoomPercent);
    setStyleSheet(sheet);
    // The card draws the dialog's background and rounding; the pages' bands sit inside it.
    const int radius = qRound(ket::Appearance::instance().scaled(
        ket::dialogRecipe(ket::Appearance::instance().theme()).radius));
    const QString cardSheet =
        sheet +
        QStringLiteral("QWidget[remoteRole=\"dialogPage\"], QFrame[remoteRole=\"modalHeader\"],"
                       " QWidget[remoteRole=\"modalBody\"] { background: transparent; }"
                       " QFrame[remoteRole=\"modalFooter\"] { border-bottom-left-radius: %1px;"
                       " border-bottom-right-radius: %1px; }")
            .arg(radius);
    // On the bodies only: a sheet on the dialog would repolish the kit's own header and
    // drop the title's type.
    for (ket::Dialog* dialog : {signInDialog_, pairingDialog_})
        if (dialog != nullptr && dialog->body() != nullptr) {
            dialog->body()->setStyleSheet(cardSheet);
            chrome_geometry::apply(dialog, zoomPercent);
        }
    if (intro_)
        intro_->setSceneColors({QColor(colors.accentSubtle), QColor(colors.page),
                                QColor(colors.border), QColor(colors.strong), QColor(colors.panel),
                                QColor(colors.muted), dark});
    chrome_geometry::apply(this, zoomPercent);
    applyingDesignSystemStyle_ = false;
}

void RemoteControlView::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::ApplicationFontChange ||
        event->type() == QEvent::FontChange)
        applyDesignSystemStyle();
    QWidget::changeEvent(event);
}

QWidget* RemoteControlView::createSignInPage() {
    auto* body = new QWidget;
    auto* form = new QVBoxLayout(body);
    chrome_geometry::margins(form, 0, 0, 0, 0);
    chrome_geometry::spacing(form, 12);
    // Rich text for the mock's 1.4 line height, which a style sheet cannot set on a label.
    auto* description =
        label(QStringLiteral("<p style=\"line-height:140%\">Continue in your browser to sign in "
                             "with your KetSuite account. KetPlus never sees your password.</p>"),
              "description");
    description->setTextFormat(Qt::RichText);
    description->setObjectName(QStringLiteral("remoteSignInDescription"));
    form->addWidget(description);
    signInNotice_ = label({}, "noticeText");
    signInNotice_->setObjectName(QStringLiteral("remoteSignInNotice"));
    signInNotice_->hide();
    form->addWidget(signInNotice_);
    return body;
}

QWidget* RemoteControlView::createControlPage() {
    auto* scroll = new QScrollArea;
    scroll->setProperty("remoteRole", QStringLiteral("viewport"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* page = new QWidget;
    page->setProperty("remoteRole", QStringLiteral("page"));
    auto* content = new QVBoxLayout(page);
    chrome_geometry::margins(content, 42, 36, 42, 38);
    chrome_geometry::spacing(content, 0);

    auto* context = new QFrame;
    context->setProperty("remoteRole", QStringLiteral("contextBar"));
    auto* contextRow = new QHBoxLayout(context);
    chrome_geometry::margins(contextRow, 28, 0, 28, 0);
    contextRow->addWidget(label(QStringLiteral("CONNECTIONS"), "context"));
    contextRow->addStretch();
    context->hide();
    content->addWidget(context);

    pageHeader_ = new QFrame;
    pageHeader_->setProperty("remoteRole", QStringLiteral("pageHeader"));
    auto* header = new QHBoxLayout(pageHeader_);
    chrome_geometry::margins(header, 0, 0, 0, 27);
    chrome_geometry::spacing(header, 18);
    auto* heading = new QVBoxLayout;
    chrome_geometry::spacing(heading, 7);
    auto* pageTitle = label(QStringLiteral("Connections"), "connectionsTitle");
    pageTitle->setObjectName(QStringLiteral("remotePageTitle"));
    heading->addWidget(pageTitle);
    auto* pageSubtitle = label(QStringLiteral("Phones that access this Mac and Linux machines "
                                              "this desktop can use."),
                               "description");
    pageSubtitle->setObjectName(QStringLiteral("remotePageSubtitle"));
    heading->addWidget(pageSubtitle);
    header->addLayout(heading, 1);
    pairButton_ = new ket::Button(QStringLiteral("Add"), ket::ButtonVariant::Primary);
    pairButton_->setObjectName(QStringLiteral("remotePairButton"));
    pairButton_->setProperty("remotePrimary", true);
    pairButton_->setIcon(symbolIcon(plusIcon, symbol_icon::primaryButtonInk()));
    chrome_geometry::iconSize(pairButton_, QSize(15, 15));
    pairButton_->setEnabled(false);
    connect(pairButton_, &QAbstractButton::clicked, this, [this] {
        QMenu menu(pairButton_);
        // The mock's Add menu.
        auto* phone = menu.addAction(tr("Pair a phone or tablet"));
        phone->setToolTip(tr("Scan a code with the KetPlus App"));
        // Linux pairing goes through its own account, so only phones wait for this Mac's relay.
        phone->setEnabled(!offline_ && !requestInFlight_);
        auto* linux = menu.addAction(tr("Connect a Linux machine"));
        linux->setToolTip(tr("Through the KetPlus Relay"));
        const auto* chosen = menu.exec(pairButton_->mapToGlobal(QPoint(0, pairButton_->height())));
        if (chosen == phone)
            createPairing();
        else if (chosen == linux && linuxMachines_)
            linuxMachines_->beginPairing();
    });
    header->addWidget(pairButton_, 0, Qt::AlignTop);
    signOutButton_ = new ket::Button(QStringLiteral("Sign out"), ket::ButtonVariant::Ghost);
    signOutButton_->setObjectName(QStringLiteral("remoteSignOutButton"));
    signOutButton_->setProperty("remoteTertiary", true);
    signOutButton_->setIcon(symbolIcon(
        logOutIcon, ket::Tokens::color(u"text-muted", ket::Appearance::instance().theme()), 14));
    chrome_geometry::iconSize(signOutButton_, QSize(14, 14));
    signOutButton_->hide();
    connect(signOutButton_, &QAbstractButton::clicked, this, &RemoteControlView::signOut);
    chrome_geometry::maximumWidth(pageHeader_, 1000);
    pageHeader_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    pageHeader_->hide();
    auto* headerHost = new QWidget;
    headerHost->setProperty("remoteRole", QStringLiteral("page"));
    auto* headerHostLayout = new QHBoxLayout(headerHost);
    chrome_geometry::margins(headerHostLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(headerHostLayout, 0);
    headerHostLayout->addStretch(1);
    headerHostLayout->addWidget(pageHeader_, 22);
    headerHostLayout->addStretch(1);
    content->addWidget(headerHost);

    auto* body = new QWidget;
    body->setProperty("remoteRole", QStringLiteral("page"));
    chrome_geometry::maximumWidth(body, 1000);
    body->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* bodyLayout = new QVBoxLayout(body);
    chrome_geometry::margins(bodyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(bodyLayout, 16);

    notice_ = label({}, "noticeText");
    notice_->setObjectName(QStringLiteral("remoteNotice"));
    notice_->hide();
    bodyLayout->addWidget(notice_);

    unlinkedContent_ = new QWidget;
    unlinkedContent_->setObjectName(QStringLiteral("remoteUnlinkedContent"));
    auto* unlinkedLayout = new QVBoxLayout(unlinkedContent_);
    chrome_geometry::margins(unlinkedLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(unlinkedLayout, 0);
    chrome_geometry::margins(unlinkedLayout, 24, 28, 24, 32);
    intro_ = new RemoteConnectionsIntro(unlinkedContent_);
    welcomeNotice_ = intro_->notice();
    signInPromptButton_ = intro_->signInButton();
    connect(intro_, &RemoteConnectionsIntro::signInRequested, this, [this] {
        setSignInNotice({});
        if (!signInDialog_->isOpen())
            signInDialog_->open();
        signInButton_->setFocus();
    });
    unlinkedLayout->addWidget(intro_);
    unlinkedLayout->addStretch();
    bodyLayout->addWidget(unlinkedContent_, 1);

    linkedContent_ = new QWidget;
    linkedContent_->setObjectName(QStringLiteral("remoteLinkedContent"));
    linkedContent_->hide();
    auto* linkedLayout = new QVBoxLayout(linkedContent_);
    chrome_geometry::margins(linkedLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(linkedLayout, 16);

    auto* overview = new QFrame(linkedContent_);
    overview->setObjectName(QStringLiteral("remoteConnectionOverview"));
    overview->setProperty("remoteRole", QStringLiteral("connectionOverview"));
    auto* overviewLayout = new QHBoxLayout(overview);
    chrome_geometry::margins(overviewLayout, 0, 20, 0, 34);
    chrome_geometry::spacing(overviewLayout, 38);
    auto* computer = new QFrame(overview);
    computer->setProperty("remoteRole", QStringLiteral("deviceIllustration"));
    chrome_geometry::fixedSize(computer, 83, 68);
    auto* computerLayout = new QVBoxLayout(computer);
    auto* computerIcon = new SymbolIconLabel(monitorIcon, 44, computer);
    computerIcon->setAlignment(Qt::AlignCenter);
    computerLayout->addWidget(computerIcon);
    overviewLayout->addWidget(computer);
    auto* overviewCopy = new QVBoxLayout;
    chrome_geometry::spacing(overviewCopy, 8);
    overviewCopy->addWidget(label(QStringLiteral("This Mac"), "overviewTitle"));
    overviewDescription_ = label(QStringLiteral("Ready for your connected devices"), "description");
    overviewDescription_->setObjectName(QStringLiteral("remoteOverviewDescription"));
    chrome_geometry::maximumWidth(overviewDescription_, 460);
    overviewCopy->addWidget(overviewDescription_);
    auto* statusRow = new QHBoxLayout;
    overviewStatus_ = badge(QStringLiteral("Connected"), QStringLiteral("positive"));
    overviewStatus_->setObjectName(QStringLiteral("remoteOverviewStatus"));
    statusRow->addWidget(overviewStatus_);
    statusRow->addStretch();
    overviewCopy->addLayout(statusRow);
    overviewLayout->addLayout(overviewCopy, 1);
    linkedLayout->addWidget(overview);

    auto* devicesHeader = new QHBoxLayout;
    devicesHeader->addWidget(label(QStringLiteral("Phones & tablets"), "sectionTitle"));
    devicesHeader->addStretch();
    deviceCount_ = label(QStringLiteral("0 paired"), "sectionCount");
    devicesHeader->addWidget(deviceCount_);
    linkedLayout->addLayout(devicesHeader);

    auto* devices = new QFrame;
    devices->setObjectName(QStringLiteral("remoteDevicesPanel"));
    devices->setProperty("remoteRole", QStringLiteral("surface"));
    devicesLayout_ = new QVBoxLayout(devices);
    chrome_geometry::margins(devicesLayout_, 0, 0, 0, 0);
    chrome_geometry::spacing(devicesLayout_, 0);
    devicesEmpty_ = label(QStringLiteral("No devices connected yet."), "emptyText");
    devicesEmpty_->setObjectName(QStringLiteral("remoteDevicesEmpty"));
    devicesEmpty_->setAlignment(Qt::AlignCenter);
    chrome_geometry::minimumHeight(devicesEmpty_, 118);
    devicesLayout_->addWidget(devicesEmpty_);
    linkedLayout->addWidget(devices);
    // What paired devices did on this Mac (#86).
    activity_ = new RemoteActivitySection(daemon_, linkedContent_);
    linkedLayout->addWidget(activity_);

    // The account and history close the page, below Linux machines, as in the mock.
    linkedFooter_ = new QWidget;
    linkedFooter_->setObjectName(QStringLiteral("remoteLinkedFooter"));
    auto* footerLayout = new QVBoxLayout(linkedFooter_);
    chrome_geometry::margins(footerLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(footerLayout, 0);
    linkedFooter_->hide();
    auto* account = new QWidget(linkedFooter_);
    account->setObjectName(QStringLiteral("remoteAccountStrip"));
    auto* accountLayout = new QHBoxLayout(account);
    chrome_geometry::margins(accountLayout, 2, 21, 2, 21);
    chrome_geometry::spacing(accountLayout, 11);
    auto* avatar = badge(QStringLiteral("AN"), QStringLiteral("positive"));
    accountLayout->addWidget(avatar);
    auto* accountCopy = new QVBoxLayout;
    chrome_geometry::spacing(accountCopy, 2);
    accountCopy->addWidget(label(QStringLiteral("An Nguyen"), "accountName"));
    auto* accountMeta =
        label(QStringLiteral("an@example.test · Personal workspace"), "accountMeta");
    accountMeta->setWordWrap(false); // one line, as the mock
    accountCopy->addWidget(accountMeta);
    accountLayout->addLayout(accountCopy);
    accountLayout->addStretch();
    accountLayout->addWidget(signOutButton_);
    footerLayout->addWidget(account);
    auto* history = new QWidget(linkedFooter_);
    auto* historyLayout = new QVBoxLayout(history);
    chrome_geometry::margins(historyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(historyLayout, 7);
    auto* historyButton =
        new ket::Button(QStringLiteral("About your history"), ket::ButtonVariant::Ghost, history);
    historyButton->setObjectName(QStringLiteral("remoteHistoryButton"));
    historyButton->setProperty("remoteTertiary", true);
    historyButton->setIcon(symbolIcon(
        shieldIcon, ket::Tokens::color(u"text-muted", ket::Appearance::instance().theme()), 14));
    chrome_geometry::iconSize(historyButton, QSize(14, 14));
    historyButton->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    historyLayout->addWidget(historyButton, 0, Qt::AlignLeft);
    auto* historyCopy =
        label(QStringLiteral("Full history is stored on this Mac. Recent content is synchronized "
                             "through the cloud for up to 24 hours. Connected devices may keep an "
                             "account-scoped local cache.\n\nOlder history requires this Mac to be "
                             "available unless already cached."),
              "trustNote");
    historyCopy->setObjectName(QStringLiteral("remoteHistoryCopy"));
    chrome_geometry::maximumWidth(historyCopy, 620);
    historyCopy->hide();
    historyLayout->addWidget(historyCopy);
    connect(historyButton, &QAbstractButton::clicked, historyCopy,
            [historyCopy] { historyCopy->setVisible(!historyCopy->isVisible()); });
    footerLayout->addWidget(history);

    pairingPanel_ = new QFrame;
    pairingPanel_->setObjectName(QStringLiteral("remotePairingPanel"));
    pairingPanel_->setProperty("remoteRole", QStringLiteral("dialogPage"));
    auto* pairing = new QVBoxLayout(pairingPanel_);
    chrome_geometry::margins(pairing, 0, 0, 0, 0);
    chrome_geometry::spacing(pairing, 0);
    auto* pairingHeader = new QFrame(pairingPanel_);
    pairingHeader->setProperty("remoteRole", QStringLiteral("modalHeader"));
    auto* pairingHeading = new QHBoxLayout;
    chrome_geometry::margins(pairingHeading, 22, 14, 14, 14);
    chrome_geometry::spacing(pairingHeading, 12);
    pairingDialogTitle_ = label(QStringLiteral("Connect your phone"), "modalTitle");
    pairingDialogTitle_->setObjectName(QStringLiteral("remotePairingDialogTitle"));
    pairingHeading->addWidget(pairingDialogTitle_);
    pairingHeading->addStretch();
    auto* closePairing =
        new ket::IconButton(ket::Icons::icon(u"close"), tr("Close"), pairingHeader);
    closePairing->setObjectName(QStringLiteral("remotePairingCloseButton"));
    closePairing->setProperty("remoteTertiary", true);
    closePairing->setToolTip(QStringLiteral("Close"));
    closePairing->setAccessibleName(QStringLiteral("Close QR code"));
    chrome_geometry::fixedSize(closePairing, 30, 30);
    connect(closePairing, &QAbstractButton::clicked, this, [this] { pairingDialog_->reject(); });
    pairingHeading->addWidget(closePairing);
    pairingHeader->setLayout(pairingHeading);
    pairing->addWidget(pairingHeader);

    auto* pairingBody = new QWidget(pairingPanel_);
    pairingBody->setProperty("remoteRole", QStringLiteral("modalBody"));
    auto* pairingBodyLayout = new QVBoxLayout(pairingBody);
    chrome_geometry::margins(pairingBodyLayout, 22, 21, 22, 22);
    chrome_geometry::spacing(pairingBodyLayout, 0);
    pairingState_ = label(QStringLiteral("Take your workspace with you."), "pairTitle");
    pairingState_->setObjectName(QStringLiteral("remotePairingState"));
    pairingState_->setAlignment(Qt::AlignCenter);
    pairingBodyLayout->addWidget(pairingState_);
    pairingHelp_ =
        label(QStringLiteral("In KetPlus on your phone, choose Scan QR code."), "description");
    pairingHelp_->setObjectName(QStringLiteral("remotePairingHelp"));
    pairingHelp_->setAlignment(Qt::AlignCenter);
    chrome_geometry::addSpacing(pairingBodyLayout, 8);
    pairingBodyLayout->addWidget(pairingHelp_);
    pairingScanSection_ = new QWidget(pairingBody);
    auto* scanLayout = new QVBoxLayout(pairingScanSection_);
    chrome_geometry::margins(scanLayout, 0, 23, 0, 0);
    chrome_geometry::spacing(scanLayout, 0);
    qrCode_ = label({}, "qr", pairingScanSection_);
    qrCode_->setObjectName(QStringLiteral("remotePairingQr"));
    // Locked KétAtlas geometry: the complete white QR surface is 204 logical px.
    chrome_geometry::fixedSize(qrCode_, 204, 204);
    qrCode_->setAlignment(Qt::AlignCenter);
    scanLayout->addWidget(qrCode_, 0, Qt::AlignHCenter);
    pairingTimer_ = label(QStringLiteral("Code expires shortly"), "description");
    pairingTimer_->setObjectName(QStringLiteral("remotePairingTimer"));
    pairingTimer_->setAlignment(Qt::AlignCenter);
    chrome_geometry::addSpacing(scanLayout, 11);
    scanLayout->addWidget(pairingTimer_);
    pairingSteps_ = label(QStringLiteral("1.  Scan this code.\n"
                                         "2.  Compare the confirmation codes.\n"
                                         "3.  Approve this device on your Mac."),
                          "pairSteps");
    pairingSteps_->setObjectName(QStringLiteral("remotePairingSteps"));
    pairingSteps_->setWordWrap(false);
    pairingSteps_->setMinimumSize(370, 74);
    chrome_geometry::maximumWidth(pairingSteps_, 390);
    chrome_geometry::addSpacing(scanLayout, 19);
    scanLayout->addWidget(pairingSteps_, 0, Qt::AlignHCenter);
    pairingBodyLayout->addWidget(pairingScanSection_);
    pairingConfirmationCode_ = label({}, "pairCode");
    pairingConfirmationCode_->setObjectName(QStringLiteral("remotePairingConfirmationCode"));
    pairingConfirmationCode_->setAlignment(Qt::AlignCenter);
    pairingConfirmationCode_->hide();
    pairingBodyLayout->addWidget(pairingConfirmationCode_);
    pairingCode_ = new ket::TextField;
    pairingCode_->setReadOnly(true);
    pairingCode_->setObjectName(QStringLiteral("remotePairingCode"));
    pairingCode_->hide();
    pairingBodyLayout->addWidget(pairingCode_);
    deviceClaim_ = label({}, "pairFacts");
    deviceClaim_->setObjectName(QStringLiteral("remoteDeviceClaim"));
    deviceClaim_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    deviceClaim_->hide();
    pairingBodyLayout->addWidget(deviceClaim_);
    pairingNotice_ = label({}, "noticeText");
    pairingNotice_->setObjectName(QStringLiteral("remotePairingNotice"));
    pairingNotice_->setProperty("remoteFailure", true);
    pairingNotice_->hide();
    pairingBodyLayout->addWidget(pairingNotice_);
    pairingDemoNoteHost_ = new QWidget(pairingBody);
    auto* demoNoteLayout = new QVBoxLayout(pairingDemoNoteHost_);
    chrome_geometry::margins(demoNoteLayout, 0, 17, 0, 0);
    chrome_geometry::spacing(demoNoteLayout, 0);
    pairingDemoNote_ =
        label(QStringLiteral("Closing or declining cancels this pairing ticket."), "description");
    pairingDemoNote_->setObjectName(QStringLiteral("remotePairingDemoNote"));
    pairingDemoNote_->setAlignment(Qt::AlignCenter);
    demoNoteLayout->addWidget(pairingDemoNote_);
    pairingBodyLayout->addWidget(pairingDemoNoteHost_);
    pairing->addWidget(pairingBody);

    auto* pairingFooter = new QFrame(pairingPanel_);
    pairingFooter->setProperty("remoteRole", QStringLiteral("modalFooter"));
    auto* pairingActions = new QHBoxLayout(pairingFooter);
    chrome_geometry::margins(pairingActions, 22, 13, 22, 13);
    chrome_geometry::spacing(pairingActions, 8);
    pairingActions->addStretch();
    pairingCancelButton_ =
        new ket::Button(QStringLiteral("Close"), ket::ButtonVariant::Secondary, pairingFooter);
    pairingCancelButton_->setObjectName(QStringLiteral("remotePairingCancelButton"));
    connect(pairingCancelButton_, &QAbstractButton::clicked, this, [this] {
        if (pairingCancelButton_->property("pairAction").toString() == QStringLiteral("expire")) {
            showPairing({{QStringLiteral("status"), QStringLiteral("expired")}});
            return;
        }
        pairingDialog_->reject();
    });
    pairingActions->addWidget(pairingCancelButton_);
    approveButton_ =
        new ket::Button(QStringLiteral("Allow this device"), ket::ButtonVariant::Primary);
    approveButton_->setObjectName(QStringLiteral("remoteApproveButton"));
    approveButton_->setProperty("remotePrimary", true);
    approveButton_->setEnabled(false);
    connect(approveButton_, &QAbstractButton::clicked, this, [this] {
        const QString action = approveButton_->property("pairAction").toString();
        if (action == QStringLiteral("retry")) {
            pairingDialog_->reject();
            QTimer::singleShot(0, this, &RemoteControlView::createPairing);
            return;
        }
        if (action == QStringLiteral("cancelRetry")) {
            this->closePairing();
            return;
        }
        if (action == QStringLiteral("simulate")) {
            showPairing({{QStringLiteral("status"), QStringLiteral("claimed")},
                         {QStringLiteral("claimDigest"), QStringLiteral("482619")},
                         {QStringLiteral("claimedDevice"),
                          QJsonObject{{QStringLiteral("displayName"), QStringLiteral("Pixel 9")},
                                      {QStringLiteral("platform"), QStringLiteral("android")}}}},
                        QStringLiteral("482619"));
            return;
        }
        approvePairing();
    });
    pairingActions->addWidget(approveButton_);
    pairing->addWidget(pairingFooter);
    pairingDialog_ = new ket::Dialog(QStringLiteral("Connect your phone"), this);
    pairingDialog_->setObjectName(QStringLiteral("remotePairingDialog"));
    pairingDialog_->setChromeless(true);
    pairingDialog_->setCardWidth(500);
    // Every way the card closes ends the ticket, including after approval: the card ignores
    // a reject once it is closed, so nothing else would clear the shown claim.
    connect(pairingDialog_, &ket::Dialog::finished, this, &RemoteControlView::closePairing);
    pairingDialog_->setBody(pairingPanel_);
    bodyLayout->addWidget(linkedContent_);
    linuxMachines_ = new LinuxMachinesSection(body);
    // The page's Add menu shows only once this Mac's relay is linked.
    linuxMachines_->setPairingEntryShown(!configured_);
    connect(linuxMachines_, &LinuxMachinesSection::machinesChanged, this,
            &RemoteControlView::machinesChanged);
    bodyLayout->addWidget(linuxMachines_);
    bodyLayout->addWidget(linkedFooter_);
    bodyLayout->addStretch(1);
    auto* bodyHost = new QWidget;
    bodyHost->setProperty("remoteRole", QStringLiteral("page"));
    auto* bodyHostLayout = new QHBoxLayout(bodyHost);
    chrome_geometry::margins(bodyHostLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(bodyHostLayout, 0);
    bodyHostLayout->addStretch(1);
    bodyHostLayout->addWidget(body, 22);
    bodyHostLayout->addStretch(1);
    content->addWidget(bodyHost, 1);
    scroll->setWidget(page);
    return scroll;
}

void RemoteControlView::activate() {
    if (linuxMachines_)
        linuxMachines_->activate();
    refreshStatus();
    if (!ticketId_.isEmpty())
        refreshPairing();
}

void RemoteControlView::setBusy(const bool busy) {
    requestInFlight_ = busy;
    signInButton_->setEnabled(!busy);
    pairButton_->setEnabled(configured_);
    signOutButton_->setEnabled(!busy && configured_);
    const QString pairAction = approveButton_->property("pairAction").toString();
    const bool actionable = pairingState_->property("claimReady").toBool() ||
                            pairAction == QStringLiteral("retry") ||
                            pairAction == QStringLiteral("simulate");
    approveButton_->setEnabled(!busy && !ticketId_.isEmpty() && actionable);
}

void RemoteControlView::signOut() {
    if (requestInFlight_ || !configured_)
        return;
    if (demoMode_) {
        closePairing();
        devicesLoaded_ = false;
        applyStatus({{QStringLiteral("configured"), false}});
        return;
    }
    if (runDecisionDialog(
            this, QStringLiteral("remoteSignOutDialog"), QStringLiteral("Sign out of Connections?"),
            QStringLiteral("Remote access from this Mac will be disconnected. Your local "
                           "conversations and running Agents stay on this Mac."),
            QStringLiteral("Signing out is not device revocation. Revoke an untrusted device "
                           "explicitly."),
            QStringLiteral("Cancel"), QStringLiteral("Sign out"), true,
            logOutIcon) != DialogChoice::Confirm)
        return;
    setBusy(true);
    daemon_->call(QStringLiteral("remote.sign_out"), {}, {}, -1,
                  [this](const QJsonObject& result, const QString& error) {
                      setBusy(false);
                      if (!error.isEmpty()) {
                          setNotice(error, true);
                          return;
                      }
                      closePairing();
                      devicesLoaded_ = false;
                      renderDevices({});
                      applyStatus(result);
                  });
}

void RemoteControlView::setNotice(const QString& text, const bool failure) {
    notice_->setText(text);
    notice_->setProperty("remoteFailure", failure);
    notice_->style()->unpolish(notice_);
    notice_->style()->polish(notice_);
    notice_->setVisible(configured_ && !text.isEmpty());
    welcomeNotice_->setText(text);
    welcomeNotice_->setProperty("remoteFailure", failure);
    welcomeNotice_->style()->unpolish(welcomeNotice_);
    welcomeNotice_->style()->polish(welcomeNotice_);
    welcomeNotice_->setVisible(!configured_ && !text.isEmpty());
}

void RemoteControlView::setSignInNotice(const QString& text, const bool failure) {
    signInNotice_->setText(text);
    signInNotice_->setProperty("remoteFailure", failure);
    signInNotice_->style()->unpolish(signInNotice_);
    signInNotice_->style()->polish(signInNotice_);
    signInNotice_->setVisible(!text.isEmpty());
}

void RemoteControlView::resetSignInButton() {
    signInButton_->setText(QStringLiteral("Continue in browser"));
}

void RemoteControlView::signIn() {
    if (requestInFlight_ || signInWaiting_)
        return;
    if (demoMode_) {
        setSignInNotice({});
        signInDialog_->accept();
        applyStatus({{QStringLiteral("configured"), true}});
        return;
    }
    setBusy(true);
    setSignInNotice({});
    daemon_->call(
        QStringLiteral("remote.sign_in"), {}, {}, -1,
        [this](const QJsonObject& result, const QString& error) {
            setBusy(false);
            const QUrl page(result.value(QStringLiteral("authorization_url")).toString());
            if (!error.isEmpty() || !page.isValid() || !QDesktopServices::openUrl(page)) {
                setSignInNotice(error.isEmpty() ? QStringLiteral("Unable to open the sign-in page.")
                                                : error,
                                true);
                return;
            }
            signInWaiting_ = true;
            signInButton_->setEnabled(false);
            signInButton_->setText(QStringLiteral("Waiting for your browser…"));
            setSignInNotice(QStringLiteral("Finish signing in in your browser, then return here."));
        });
}

void RemoteControlView::cancelSignIn() {
    if (!signInWaiting_)
        return;
    signInWaiting_ = false;
    resetSignInButton();
    signInButton_->setEnabled(!requestInFlight_);
    daemon_->call(QStringLiteral("remote.sign_in.cancel"), {}, {}, -1,
                  [](const QJsonObject&, const QString&) {});
}

void RemoteControlView::refreshStatus() {
    if (demoMode_)
        return;
    if (!daemon_->isReady() || requestInFlight_)
        return;
    daemon_->call(QStringLiteral("remote.status"), {}, {}, -1,
                  [this](const QJsonObject& result, const QString& error) {
                      if (!error.isEmpty()) {
                          setNotice(QStringLiteral("Connections are temporarily unavailable."),
                                    true);
                          return;
                      }
                      applyStatus(result);
                  });
    if (!ticketId_.isEmpty())
        refreshPairing();
}

void RemoteControlView::applyStatus(const QJsonObject& status) {
    const bool wasConfigured = configured_;
    configured_ = status.value(QStringLiteral("configured")).toBool();
    if (signInWaiting_ &&
        (configured_ || !status.value(QStringLiteral("sign_in_pending")).toBool())) {
        signInWaiting_ = false;
        resetSignInButton();
        signInButton_->setEnabled(!requestInFlight_);
        if (configured_) {
            setSignInNotice({});
            signInDialog_->accept();
        } else {
            const QString error = status.value(QStringLiteral("sign_in_error")).toString();
            setSignInNotice(error.isEmpty() ? QStringLiteral("Sign-in did not finish. Try again.")
                                            : error,
                            true);
        }
    }
    pageHeader_->setVisible(configured_);
    unlinkedContent_->setVisible(!configured_);
    linkedContent_->setVisible(configured_);
    linkedFooter_->setVisible(configured_);
    signOutButton_->setVisible(configured_);
    const QString lastError = status.value(QStringLiteral("last_error")).toString();
    offline_ = configured_ && !lastError.isEmpty();
    pairButton_->setEnabled(configured_);
    if (linuxMachines_)
        linuxMachines_->setPairingEntryShown(!configured_);
    overviewDescription_->setText(offline_ ? QStringLiteral("Working locally")
                                           : QStringLiteral("Ready for your connected devices"));
    overviewStatus_->setText(offline_ ? QStringLiteral("Offline") : QStringLiteral("Connected"));
    overviewStatus_->setProperty("remoteTone",
                                 offline_ ? QStringLiteral("warning") : QStringLiteral("positive"));
    overviewStatus_->style()->unpolish(overviewStatus_);
    overviewStatus_->style()->polish(overviewStatus_);
    intro_->setExpired(!configured_ && !lastError.isEmpty());
    if (offline_)
        setNotice(QStringLiteral("This Mac is offline. Your local sessions keep running. Remote "
                                 "updates resume when connected."));
    else
        setNotice({});
    if (configured_ && (!wasConfigured || !devicesLoaded_))
        refreshDevices();
    // Demo data has no audit; a real sign-in shows it, signing out forgets it.
    if (configured_ && !demoMode_)
        activity_->refresh(!wasConfigured);
    else if (!configured_)
        activity_->clear();
}

void RemoteControlView::refreshDevices() {
    if (!configured_ || devicesRequestInFlight_)
        return;
    if (demoMode_) {
        devicesLoaded_ = true;
        renderDevices({});
        return;
    }
    devicesRequestInFlight_ = true;
    if (devicesEmpty_)
        devicesEmpty_->setText(QStringLiteral("Refreshing devices…"));
    daemon_->call(QStringLiteral("remote.devices"), {}, {}, -1,
                  [this](const QJsonObject& result, const QString& error) {
                      devicesRequestInFlight_ = false;
                      if (!error.isEmpty()) {
                          if (devicesEmpty_)
                              devicesEmpty_->setText(
                                  QStringLiteral("Devices are temporarily unavailable."));
                          return;
                      }
                      devicesLoaded_ = true;
                      renderDevices(result.value(QStringLiteral("devices")).toArray());
                  });
}

void RemoteControlView::renderDevices(const QJsonArray& devices) {
    while (QLayoutItem* item = devicesLayout_->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    devicesEmpty_ = nullptr;
    deviceCount_->setText(QStringLiteral("%1 paired").arg(devices.size()));
    QHash<QString, QString> names;
    for (const QJsonValue& value : devices) {
        const QJsonObject device = value.toObject();
        names.insert(
            device.value(QStringLiteral("id")).toString(),
            device.value(QStringLiteral("displayName")).toString(QStringLiteral("KetPlus Remote")));
    }
    activity_->setDeviceNames(names);
    if (devices.isEmpty()) {
        devicesEmpty_ = label(QStringLiteral("No devices connected yet."), "emptyText");
        devicesEmpty_->setObjectName(QStringLiteral("remoteDevicesEmpty"));
        devicesEmpty_->setAlignment(Qt::AlignCenter);
        chrome_geometry::minimumHeight(devicesEmpty_, 118);
        devicesLayout_->addWidget(devicesEmpty_);
        return;
    }
    for (const QJsonValue& value : devices) {
        const QJsonObject device = value.toObject();
        auto* row = new QFrame;
        row->setProperty("remoteRole", QStringLiteral("deviceRow"));
        auto* rowLayout = new QHBoxLayout(row);
        chrome_geometry::margins(rowLayout, 22, 24, 22, 24);
        chrome_geometry::spacing(rowLayout, 18);
        auto* iconSurface = new QFrame(row);
        iconSurface->setProperty("remoteRole", QStringLiteral("deviceIconSurface"));
        chrome_geometry::fixedSize(iconSurface, 40, 40);
        auto* iconLayout = new QVBoxLayout(iconSurface);
        chrome_geometry::margins(iconLayout, 10, 10, 10, 10);
        auto* icon = new SymbolIconLabel(phoneIcon, 20, iconSurface);
        icon->setAlignment(Qt::AlignCenter);
        iconLayout->addWidget(icon);
        rowLayout->addWidget(iconSurface);
        auto* copy = new QVBoxLayout;
        chrome_geometry::spacing(copy, 2);
        const QString name =
            device.value(QStringLiteral("displayName")).toString(QStringLiteral("KetPlus Remote"));
        copy->addWidget(label(name, "deviceName"));
        const QString platform = device.value(QStringLiteral("platform")).toString().toUpper();
        copy->addWidget(label(platform.isEmpty()
                                  ? QStringLiteral("KetPlus App · Paired")
                                  : QStringLiteral("KetPlus App · Paired · %1").arg(platform),
                              "deviceMeta"));
        rowLayout->addLayout(copy, 1);
        auto* details = new ket::Button(QStringLiteral("Details"), ket::ButtonVariant::Ghost);
        details->setProperty("remoteTertiary", true);
        const QString deviceId = device.value(QStringLiteral("id")).toString();
        connect(details, &QAbstractButton::clicked, this,
                [this, deviceId, name] { showDeviceDetails(deviceId, name); });
        rowLayout->addWidget(details);
        auto* revoke =
            new ket::Button(QStringLiteral("Revoke access"), ket::ButtonVariant::GhostDanger);
        revoke->setProperty("remoteDanger", true);
        revoke->setIcon(symbolIcon(
            unplugIcon, ket::Tokens::color(u"danger", ket::Appearance::instance().theme()), 14));
        chrome_geometry::iconSize(revoke, QSize(14, 14));
        revoke->setEnabled(!deviceId.isEmpty());
        connect(revoke, &QAbstractButton::clicked, this,
                [this, deviceId] { revokeDevice(deviceId); });
        rowLayout->addWidget(revoke);
        devicesLayout_->addWidget(row);
    }
}

void RemoteControlView::showDeviceDetails(const QString& deviceId, const QString& displayName) {
    // Its details are what it did on this Mac (#86), with the way to revoke it.
    activity_->showDevice(deviceId, displayName, [this, deviceId] { revokeDevice(deviceId); });
}

void RemoteControlView::createPairing() {
    if (requestInFlight_ || !configured_)
        return;
    const quint64 pairingEpoch = ++pairingEpoch_;
    setBusy(true);
    pairingPanel_->show();
    if (!pairingDialog_->isOpen())
        pairingDialog_->open();
    pairingDialogTitle_->setText(QStringLiteral("Connect your phone"));
    pairingState_->setText(QStringLiteral("Take your workspace with you."));
    pairingHelp_->setText(QStringLiteral("In KetPlus on your phone, choose Scan QR code."));
    pairingState_->setProperty("claimReady", false);
    pairingScanSection_->show();
    qrCode_->show();
    pairingTimer_->setText(QStringLiteral("Preparing a secure pairing code…"));
    pairingTimer_->show();
    pairingSteps_->show();
    pairingConfirmationCode_->hide();
    deviceClaim_->hide();
    pairingNotice_->hide();
    pairingDemoNoteHost_->show();
    pairingCancelButton_->setText(QStringLiteral("Close"));
    pairingCancelButton_->setProperty("pairAction", QStringLiteral("close"));
    pairingCancelButton_->show();
    approveButton_->setProperty("pairAction", QStringLiteral("approve"));
    approveButton_->hide();
    if (demoMode_) {
        ticketId_ = QStringLiteral("demo-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString code = QStringLiteral("ketplus://pair?demo=%1").arg(ticketId_);
        pairingCode_->setText(code);
        const QImage image = makePairingQrCode(code, 220);
        qrCode_->setPixmap(QPixmap::fromImage(image).scaled(
            qrCode_->size() - QSize(20, 20), Qt::KeepAspectRatio, Qt::FastTransformation));
        pairingTimer_->setText(QStringLiteral("Code expires in 04:59 · Demo snapshot"));
        approveButton_->setText(QStringLiteral("Simulate scan"));
        approveButton_->setProperty("pairAction", QStringLiteral("simulate"));
        pairingCancelButton_->setText(QStringLiteral("Expire code"));
        pairingCancelButton_->setProperty("pairAction", QStringLiteral("expire"));
        approveButton_->show();
        setBusy(false);
        pairingPanel_->layout()->activate();
        return;
    }
    daemon_->call(
        QStringLiteral("remote.pairing.create"), {}, {}, -1,
        [this, pairingEpoch](const QJsonObject& result, const QString& error) {
            if (pairingEpoch != pairingEpoch_) {
                const QString staleTicketId = result.value(QStringLiteral("ticket"))
                                                  .toObject()
                                                  .value(QStringLiteral("id"))
                                                  .toString();
                if (error.isEmpty() && !staleTicketId.isEmpty()) {
                    daemon_->call(QStringLiteral("remote.pairing.revoke"),
                                  {{QStringLiteral("ticket_id"), staleTicketId}}, {}, -1,
                                  [](const QJsonObject&, const QString&) {});
                }
                return;
            }
            setBusy(false);
            if (!error.isEmpty()) {
                if (!pairingDialog_->isVisible())
                    return;
                pairingDialogTitle_->setText(QStringLiteral("Pairing unavailable"));
                pairingState_->setText(QStringLiteral("Unable to create a pairing code"));
                pairingHelp_->setText(
                    QStringLiteral("No device was connected. Retry when this Mac is online."));
                pairingScanSection_->hide();
                qrCode_->hide();
                pairingTimer_->hide();
                pairingSteps_->hide();
                pairingDemoNoteHost_->hide();
                pairingNotice_->setText(error);
                pairingNotice_->show();
                approveButton_->setText(QStringLiteral("Generate new code"));
                approveButton_->setProperty("pairAction", QStringLiteral("retry"));
                approveButton_->show();
                approveButton_->setEnabled(true);
                pairingPanel_->layout()->activate();
                return;
            }
            const QJsonObject ticket = result.value(QStringLiteral("ticket")).toObject();
            ticketId_ = ticket.value(QStringLiteral("id")).toString();
            shownClaimDigest_.clear();
            claimChanged_ = false;
            if (!pairingDialog_->isVisible()) {
                closePairing();
                return;
            }
            const QString code = result.value(QStringLiteral("pairing_code")).toString();
            pairingCode_->setText(code);
            const QImage image = makePairingQrCode(code, 220);
            qrCode_->setPixmap(QPixmap::fromImage(image).scaled(
                qrCode_->size() - QSize(20, 20), Qt::KeepAspectRatio, Qt::FastTransformation));
            showPairing(ticket);
        });
}

void RemoteControlView::refreshPairing() {
    if (demoMode_)
        return;
    if (ticketId_.isEmpty())
        return;
    const QString ticketId = ticketId_;
    const quint64 pairingEpoch = pairingEpoch_;
    daemon_->call(QStringLiteral("remote.pairing.status"),
                  {{QStringLiteral("ticket_id"), ticketId}}, {}, -1,
                  [this, pairingEpoch, ticketId](const QJsonObject& result, const QString& error) {
                      if (pairingEpoch == pairingEpoch_ && ticketId == ticketId_ && error.isEmpty())
                          showPairing(result.value(QStringLiteral("ticket")).toObject(),
                                      result.value(QStringLiteral("confirmation_code")).toString());
                  });
}

namespace {

// "Code expires in 4:59" from a ticket's expiresAt (an ISO time or epoch milliseconds). Pairing
// status refreshes every two seconds, which keeps it current.
QString pairingExpiryText(const QJsonValue& expiresAt) {
    const QDateTime at = expiresAt.isDouble()
                             ? QDateTime::fromMSecsSinceEpoch(
                                   static_cast<qint64>(expiresAt.toDouble()), QTimeZone::UTC)
                             : QDateTime::fromString(expiresAt.toString(), Qt::ISODateWithMs);
    if (!at.isValid())
        return QStringLiteral("Code expires shortly");
    const qint64 seconds = QDateTime::currentDateTimeUtc().secsTo(at);
    if (seconds <= 0)
        return QStringLiteral("Code expired");
    return QStringLiteral("Code expires in %1:%2")
        .arg(seconds / 60)
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

void RemoteControlView::showPairing(const QJsonObject& ticket, const QString& confirmationCode) {
    const QJsonObject device = ticket.value(QStringLiteral("claimedDevice")).toObject();
    const QString claimDigest = ticket.value(QStringLiteral("claimDigest")).toString();
    // Once a code was shown, only that claim may be approved. A digest that changes or disappears
    // ends this ticket, even if a later refresh shows the first digest again.
    claimChanged_ =
        claimChanged_ || (!shownClaimDigest_.isEmpty() && claimDigest != shownClaimDigest_);
    QString state = claimChanged_ ? QStringLiteral("claim changed")
                                  : ticket.value(QStringLiteral("status")).toString();
    // The daemon gives a code only for a claim whose keys reproduce its digest (#89).
    if (state == QStringLiteral("claimed") && confirmationCode.size() != 6)
        state = QStringLiteral("claim unverified");
    const bool ready = state == QStringLiteral("claimed") && !device.isEmpty();
    // A finished ticket's QR code and secret are of no further use.
    if (state != QStringLiteral("open") && !ready) {
        pairingCode_->clear();
        qrCode_->clear();
    }
    pairingState_->setProperty("claimReady", ready);
    pairingNotice_->hide();
    if (state == QStringLiteral("open")) {
        pairingDialogTitle_->setText(QStringLiteral("Connect your phone"));
        pairingState_->setText(QStringLiteral("Take your workspace with you."));
        pairingHelp_->setText(QStringLiteral("In KetPlus on your phone, choose Scan QR code."));
        pairingScanSection_->show();
        qrCode_->show();
        pairingTimer_->setText(pairingExpiryText(ticket.value(QStringLiteral("expiresAt"))));
        pairingTimer_->show();
        pairingSteps_->show();
        pairingConfirmationCode_->hide();
        deviceClaim_->hide();
        pairingDemoNoteHost_->show();
        pairingCancelButton_->setText(QStringLiteral("Close"));
        pairingCancelButton_->setProperty("pairAction", QStringLiteral("close"));
    } else if (ready) {
        pairingDialogTitle_->setText(QStringLiteral("Confirm device"));
        pairingState_->setText(QStringLiteral("Is this your phone?"));
        pairingHelp_->setText(QStringLiteral("Compare the code on both devices."));
        pairingScanSection_->hide();
        qrCode_->hide();
        pairingTimer_->hide();
        pairingSteps_->hide();
        shownClaimDigest_ = claimDigest;
        pairingConfirmationCode_->setText(confirmationCode.left(3) + QChar(' ') +
                                          confirmationCode.mid(3, 3));
        pairingConfirmationCode_->show();
        const QString deviceName =
            device.value(QStringLiteral("displayName")).toString(QStringLiteral("Mobile device"));
        // Only what was checked: the relay does not report the phone's account.
        deviceClaim_->setText(QStringLiteral("Device     %1").arg(deviceName));
        deviceClaim_->show();
        pairingDemoNoteHost_->hide();
        pairingCancelButton_->setText(QStringLiteral("Decline"));
        pairingCancelButton_->setProperty("pairAction", QStringLiteral("close"));
        approveButton_->setText(QStringLiteral("Yes, connect this device"));
        approveButton_->setProperty("pairAction", QStringLiteral("approve"));
    } else if (state == QStringLiteral("approved")) {
        pairingDialogTitle_->setText(QStringLiteral("Device connected"));
        pairingState_->setText(QStringLiteral("Your phone is ready"));
        pairingHelp_->setText(QStringLiteral("You can now use this device to access KetPlus."));
        pairingScanSection_->hide();
        qrCode_->hide();
        pairingTimer_->hide();
        pairingSteps_->hide();
        pairingConfirmationCode_->hide();
        deviceClaim_->hide();
        pairingDemoNoteHost_->hide();
        pairingCancelButton_->setText(QStringLiteral("Close"));
        pairingCancelButton_->setProperty("pairAction", QStringLiteral("close"));
        ticketId_.clear();
        devicesLoaded_ = false;
        refreshDevices();
        QTimer::singleShot(900, pairingDialog_, &ket::Dialog::accept);
    } else if (state == QStringLiteral("expired") || state == QStringLiteral("revoked")) {
        const bool revoked = state == QStringLiteral("revoked");
        pairingDialogTitle_->setText(revoked ? QStringLiteral("Pairing cancelled")
                                             : QStringLiteral("Pairing code expired"));
        pairingState_->setText(revoked ? QStringLiteral("This pairing was cancelled")
                                       : QStringLiteral("This pairing code is no longer valid"));
        pairingHelp_->setText(QStringLiteral("No device was connected."));
        pairingScanSection_->hide();
        qrCode_->hide();
        pairingTimer_->hide();
        pairingSteps_->hide();
        pairingConfirmationCode_->hide();
        deviceClaim_->hide();
        pairingDemoNoteHost_->hide();
        pairingCancelButton_->setText(QStringLiteral("Close"));
        pairingCancelButton_->setProperty("pairAction", QStringLiteral("close"));
        approveButton_->setText(QStringLiteral("Generate new code"));
        approveButton_->setProperty("pairAction", QStringLiteral("retry"));
    } else {
        pairingDialogTitle_->setText(QStringLiteral("Pairing unavailable"));
        pairingState_->setText(QStringLiteral("Unable to continue pairing"));
        pairingHelp_->setText(QStringLiteral("No access has been granted."));
        pairingScanSection_->hide();
        qrCode_->hide();
        pairingTimer_->hide();
        pairingSteps_->hide();
        pairingConfirmationCode_->hide();
        deviceClaim_->hide();
        pairingDemoNoteHost_->hide();
        pairingNotice_->setText(QStringLiteral("Pairing status: %1").arg(state));
        pairingNotice_->show();
        pairingCancelButton_->setText(QStringLiteral("Close"));
        pairingCancelButton_->setProperty("pairAction", QStringLiteral("close"));
        approveButton_->setText(QStringLiteral("Generate new code"));
        approveButton_->setProperty("pairAction", QStringLiteral("retry"));
    }
    const bool retry = approveButton_->property("pairAction").toString() == QStringLiteral("retry");
    approveButton_->setVisible(ready || retry);
    approveButton_->setEnabled((ready || retry) && !requestInFlight_);
    pairingPanel_->layout()->activate();
}

void RemoteControlView::approvePairing() {
    if (ticketId_.isEmpty() || requestInFlight_)
        return;
    if (demoMode_) {
        showPairing({{QStringLiteral("status"), QStringLiteral("approved")},
                     {QStringLiteral("claimDigest"), shownClaimDigest_}});
        return;
    }
    setBusy(true);
    approveButton_->setText(QStringLiteral("Connecting…"));
    const QString ticketId = ticketId_;
    const quint64 pairingEpoch = pairingEpoch_;
    // The daemon approves only the claim whose code this dialog showed.
    daemon_->call(QStringLiteral("remote.pairing.approve"),
                  {{QStringLiteral("ticket_id"), ticketId},
                   {QStringLiteral("claim_digest"), shownClaimDigest_}},
                  {}, -1,
                  [this, pairingEpoch, ticketId](const QJsonObject& result, const QString& error) {
                      if (pairingEpoch != pairingEpoch_ || ticketId != ticketId_)
                          return;
                      setBusy(false);
                      if (!error.isEmpty()) {
                          approveButton_->setText(QStringLiteral("Yes, connect this device"));
                          pairingNotice_->setText(
                              QStringLiteral("Confirmation failed. No access has been granted. %1")
                                  .arg(error));
                          pairingNotice_->show();
                          return;
                      }
                      showPairing(result.value(QStringLiteral("ticket")).toObject());
                  });
}

void RemoteControlView::revokeDevice(const QString& deviceId) {
    if (deviceId.isEmpty() || requestInFlight_)
        return;
    if (runDecisionDialog(
            this, QStringLiteral("remoteRevokeDialog"), QStringLiteral("Revoke device access?"),
            QStringLiteral("This device will lose access to this Mac. Local Agent sessions keep "
                           "running."),
            QStringLiteral("The device is removed only after revocation is acknowledged."),
            QStringLiteral("Keep device"), QStringLiteral("Revoke access"), true,
            unplugIcon) != DialogChoice::Confirm) {
        return;
    }
    setBusy(true);
    setNotice(QStringLiteral("Revoking device access…"));
    daemon_->call(QStringLiteral("remote.device.revoke"), {{QStringLiteral("device_id"), deviceId}},
                  {}, -1, [this](const QJsonObject&, const QString& error) {
                      setBusy(false);
                      if (!error.isEmpty()) {
                          setNotice(QStringLiteral("Revocation failed. Access has not been "
                                                   "confirmed revoked. Retry while connected."),
                                    true);
                          return;
                      }
                      setNotice(QStringLiteral("Access revoked. This device can no longer access "
                                               "this Mac."));
                      devicesLoaded_ = false;
                      refreshDevices();
                  });
}

void RemoteControlView::closePairing() {
    ++pairingEpoch_;
    shownClaimDigest_.clear();
    claimChanged_ = false;
    pairingCode_->clear();
    qrCode_->clear();
    const QString ticketId = std::exchange(ticketId_, {});
    setBusy(false);
    if (demoMode_ || ticketId.isEmpty())
        return;
    daemon_->call(QStringLiteral("remote.pairing.revoke"),
                  {{QStringLiteral("ticket_id"), ticketId}}, {}, -1,
                  [this, ticketId](const QJsonObject&, const QString& error) {
                      if (error.isEmpty()) {
                          if (pairingDialog_->isVisible() &&
                              approveButton_->property("pairAction").toString() ==
                                  QStringLiteral("cancelRetry"))
                              pairingDialog_->accept();
                          return;
                      }
                      ticketId_ = ticketId;
                      pairingDialogTitle_->setText(QStringLiteral("Pairing cancellation pending"));
                      pairingState_->setText(QStringLiteral("Access has not been approved"));
                      pairingHelp_->setText(QStringLiteral(
                          "We could not confirm ticket cancellation while disconnected."));
                      pairingScanSection_->hide();
                      pairingConfirmationCode_->hide();
                      deviceClaim_->hide();
                      pairingDemoNoteHost_->hide();
                      pairingNotice_->setText(QStringLiteral(
                          "The ticket expires automatically. Keep this code private until then."));
                      pairingNotice_->show();
                      pairingCancelButton_->hide();
                      approveButton_->setText(QStringLiteral("Retry cancellation"));
                      approveButton_->setProperty("pairAction", QStringLiteral("cancelRetry"));
                      approveButton_->setEnabled(true);
                      approveButton_->show();
                      pairingPanel_->layout()->activate();
                      if (!pairingDialog_->isOpen())
                          pairingDialog_->open();
                  });
}

} // namespace ketplus

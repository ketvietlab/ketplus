#include "app/SettingsWorkspacePage.h"
#include "ui/ChromeGeometry.h"

#include "agent/AgentWorkspaceView.h"
#include "app/AgentConversationWidth.h"
#include "app/SettingsBrowserPage.h"
#include "app/SettingsComputerPage.h"
#include "app/SettingsDialog.h"
#include "app/SettingsUpdatesPage.h"
#include "ui/InterfaceFont.h"
#include "ui/InterfaceZoom.h"
#include "ui/LucideIcon.h"
#include "ui/Theme.h"
#include "ui/TypeScale.h"
#include "update/AppUpdater.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSizePolicy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

namespace ketplus {
namespace {

class SettingsPageContent final : public QWidget {
  public:
    using QWidget::QWidget;

    QSize sizeHint() const override {
        QSize hint = QWidget::sizeHint();
        hint.setWidth(1220);
        return hint;
    }
};

QLabel* label(const QString& text, const QString& objectName, QWidget* parent) {
    auto* value = new QLabel(text, parent);
    value->setObjectName(objectName);
    value->setWordWrap(true);
    return value;
}

QPixmap themePreview(const bool dark) {
    constexpr int width = 190;
    constexpr int height = 123;
    QPixmap pixmap(width * 2, height * 2);
    pixmap.setDevicePixelRatio(2.0);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QColor page = dark ? QColor("#191D22") : QColor("#F8F6F6");
    const QColor panel = dark ? QColor("#22272E") : QColor("#FFFFFF");
    const QColor line = dark ? QColor("#414852") : QColor("#DAD7D8");
    const QColor muted = dark ? QColor("#7E8793") : QColor("#B0ADAE");
    painter.setPen(QPen(line, 1));
    painter.setBrush(page);
    painter.drawRoundedRect(QRectF(0.5, 0.5, width - 1, height - 1), 5, 5);
    painter.fillRect(QRectF(0, 0, 47, height), panel);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor("#1746FF"));
    painter.drawRoundedRect(QRectF(8, 15, 31, 6), 3, 3);
    painter.setBrush(muted);
    for (int row = 0; row < 6; ++row)
        painter.drawRoundedRect(QRectF(8, 34 + row * 13, 25 + (row % 2) * 8, 3), 1.5, 1.5);
    painter.setBrush(panel);
    painter.drawRoundedRect(QRectF(59, 15, 118, 93), 4, 4);
    painter.setBrush(line);
    painter.drawRoundedRect(QRectF(70, 29, 67, 4), 2, 2);
    painter.setBrush(muted);
    painter.drawRoundedRect(QRectF(70, 43, 89, 3), 1.5, 1.5);
    painter.drawRoundedRect(QRectF(70, 52, 74, 3), 1.5, 1.5);
    painter.setBrush(dark ? QColor("#2C3351") : QColor("#F0F1FC"));
    painter.drawRoundedRect(QRectF(70, 67, 40, 22), 3, 3);
    painter.drawRoundedRect(QRectF(117, 67, 40, 22), 3, 3);
    painter.setBrush(line);
    painter.drawRoundedRect(QRectF(70, 98, 77, 3), 1.5, 1.5);
    return pixmap;
}

QIcon navigationIcon(const QByteArray& body, const QColor& color, const QSize& size) {
    return QIcon(lucide_icon::pixmap(body, color, size));
}

QByteArray navigationIconBody(const QString& id) {
    if (id == QStringLiteral("appearance"))
        return QByteArrayLiteral(
            R"(<circle cx="13.5" cy="6.5" r=".5"/><circle cx="17.5" cy="10.5" r=".5"/><circle cx="8.5" cy="7.5" r=".5"/><circle cx="6.5" cy="12.5" r=".5"/><path d="M12 22a10 10 0 1 0 0-20 7 7 0 0 0-7 7c0 1.66 1.34 3 3 3h1a2 2 0 0 1 2 2v1a2 2 0 0 0 2 2h1a2 2 0 0 1 2 2c0 1.66-1.34 3-3 3z"/>)");
    if (id == QStringLiteral("typography"))
        return QByteArrayLiteral(
            R"(<polyline points="4 7 4 4 20 4 20 7"/><line x1="9" x2="15" y1="20" y2="20"/><line x1="12" x2="12" y1="4" y2="20"/>)");
    if (id == QStringLiteral("account-devices"))
        return QByteArrayLiteral(
            R"(<rect width="20" height="14" x="2" y="3" rx="2"/><line x1="8" x2="16" y1="21" y2="21"/><line x1="12" x2="12" y1="17" y2="21"/>)");
    if (id == QStringLiteral("providers"))
        return QByteArrayLiteral(
            R"(<path d="m7.5 4.27 9 5.15"/><path d="M21 8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16Z"/><path d="m3.3 7 8.7 5 8.7-5"/><path d="M12 22V12"/>)");
    if (id == QStringLiteral("computer")) // Lucide mouse-pointer-click
        return QByteArrayLiteral(
            R"(<path d="M14 4.1 12 6"/><path d="m5.1 8-2.9-.8"/><path d="m6 12-1.9 2"/><path d="M7.2 2.2 8 5.1"/><path d="M9.037 9.69a.498.498 0 0 1 .653-.653l11 4.5a.5.5 0 0 1-.074.949l-4.349 1.041a1 1 0 0 0-.74.739l-1.04 4.35a.5.5 0 0 1-.95.074z"/>)");
    if (id == QStringLiteral("storage")) // Lucide database
        return QByteArrayLiteral(
            R"(<ellipse cx="12" cy="5" rx="9" ry="3"/><path d="M3 5v14a9 3 0 0 0 18 0V5"/><path d="M3 12a9 3 0 0 0 18 0"/>)");
    if (id == QStringLiteral("updates"))
        return QByteArrayLiteral(
            R"(<path d="M12 15V3"/><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"/><path d="m7 10 5 5 5-5"/>)");
    if (id == QStringLiteral("agent-models"))
        return QByteArrayLiteral(
            R"(<line x1="21" x2="14" y1="4" y2="4"/><line x1="10" x2="3" y1="4" y2="4"/><line x1="21" x2="12" y1="12" y2="12"/><line x1="8" x2="3" y1="12" y2="12"/><line x1="21" x2="16" y1="20" y2="20"/><line x1="12" x2="3" y1="20" y2="20"/><line x1="14" x2="14" y1="2" y2="6"/><line x1="8" x2="8" y1="10" y2="14"/><line x1="16" x2="16" y1="18" y2="22"/>)");
    return QByteArrayLiteral(
        R"(<path d="M3 6h18"/><path d="M19 6v14a2 2 0 0 1-2 2H7a2 2 0 0 1-2-2V6"/><path d="m8 6 1-3h6l1 3"/><line x1="10" x2="10" y1="11" y2="17"/><line x1="14" x2="14" y1="11" y2="17"/>)");
}

QToolButton* themeCard(const QString& title, const bool dark, QWidget* parent) {
    auto* card = new QToolButton(parent);
    card->setObjectName(dark ? QStringLiteral("settingsDarkThemeCard")
                             : QStringLiteral("settingsLightThemeCard"));
    card->setProperty("kvRole", QStringLiteral("settingsThemeCard"));
    card->setText(title);
    card->setIcon(QIcon(themePreview(dark)));
    chrome_geometry::iconSize(card, QSize(190, 123));
    card->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
    card->setCheckable(true);
    chrome_geometry::fixedSize(card, 208, 170);
    return card;
}

QFrame* divider(QWidget* parent) {
    auto* line = new QFrame(parent);
    line->setObjectName(QStringLiteral("settingsWorkspaceDivider"));
    line->setFrameShape(QFrame::HLine);
    chrome_geometry::fixedHeight(line, 1);
    return line;
}

QScrollArea* scrollPage(QWidget* page, QWidget* parent) {
    auto* scroll = new QScrollArea(parent);
    scroll->setObjectName(QStringLiteral("settingsWorkspaceScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(page);
    return scroll;
}

// Holds the chosen conversation width until Save, so the public dialog's Save
// and Reset cover it like every other field. It is registered with the dialog but
// never shown there: the segmented buttons on the Typography page edit it.
class ConversationWidthState final : public SettingsPage {
  public:
    ConversationWidthState()
        : SettingsPage(QStringLiteral("agent-conversation-width"),
                       QStringLiteral("Conversation width")),
          saved_(AgentConversationWidth::load()), pixels_(saved_) {}

    [[nodiscard]] int pixels() const noexcept { return pixels_; }
    void select(const int pixels) {
        if (pixels_ == pixels)
            return;
        pixels_ = pixels;
        emit changed();
    }
    [[nodiscard]] bool hasChanges() const override { return pixels_ != saved_; }
    void resetToDefaults() override { select(AgentWorkspaceView::defaultConversationWidth); }
    void apply() override {
        AgentConversationWidth::save(pixels_);
        saved_ = pixels_;
    }

  private:
    int saved_;
    int pixels_;
};

// One bordered track with the buttons inside it, so no button has to overlap a
// neighbour to share an edge, and every side of the group is always drawn.
QWidget* segmentedGroup(const QString& objectName, QWidget* parent) {
    auto* group = new QWidget(parent);
    group->setObjectName(objectName);
    group->setProperty("kvRole", QStringLiteral("segmentedGroup"));
    group->setAttribute(Qt::WA_StyledBackground, true);
    // A help line beside it may make the row taller; the track keeps its own height.
    group->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* layout = new QHBoxLayout(group);
    chrome_geometry::margins(layout, 2, 2, 2, 2);
    chrome_geometry::spacing(layout, 2);
    return group;
}

void addField(QGridLayout* grid, const int row, const QString& title, QWidget* control,
              QWidget* parent) {
    auto* titleLabel = label(title, QStringLiteral("settingsWorkspaceFieldLabel"), parent);
    grid->addWidget(titleLabel, row, 0, Qt::AlignVCenter);
    grid->addWidget(control, row, 1);
}

} // namespace

SettingsWorkspacePage::SettingsWorkspacePage(SettingsDialog* dialog,
                                             SettingsProvidersPage::Fetcher providers,
                                             QWidget* parent,
                                             SettingsClaudeCredentials::Requester credentials,
                                             SettingsStoragePage::Requester storageRequester)
    : QWidget(parent), dialog_(dialog) {
    setObjectName(QStringLiteral("settingsWorkspacePage"));
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* layout = new QVBoxLayout(this);
    chrome_geometry::margins(layout, 42, 36, 42, 38);
    chrome_geometry::spacing(layout, 0);

    auto* content = new SettingsPageContent(this);
    content->setObjectName(QStringLiteral("settingsWorkspaceContent"));
    chrome_geometry::maximumWidth(content, 1220);
    content->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* contentLayout = new QVBoxLayout(content);
    chrome_geometry::margins(contentLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(contentLayout, 0);

    auto* heading = new QWidget(content);
    heading->setObjectName(QStringLiteral("settingsWorkspaceHeading"));
    auto* headingLayout = new QHBoxLayout(heading);
    chrome_geometry::margins(headingLayout, 0, 0, 0, 0);
    auto* copy = new QWidget(heading);
    auto* copyLayout = new QVBoxLayout(copy);
    chrome_geometry::margins(copyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(copyLayout, 4);
    copyLayout->addWidget(
        label(tr("MAKE IT YOUR OWN"), QStringLiteral("settingsWorkspaceEyebrow"), copy));
    copyLayout->addWidget(label(tr("Settings"), QStringLiteral("settingsWorkspaceTitle"), copy));
    copyLayout->addWidget(label(tr("One consistent look across your entire workspace."),
                                QStringLiteral("settingsWorkspaceSubtitle"), copy));
    headingLayout->addWidget(copy, 1);
    auto* shortcut =
        label(QStringLiteral("⌘ ,"), QStringLiteral("settingsWorkspaceShortcut"), heading);
    headingLayout->addWidget(shortcut, 0, Qt::AlignTop);
    contentLayout->addWidget(heading);
    chrome_geometry::addSpacing(contentLayout, 28);

    dialog_->setParent(this);
    dialog_->setWindowFlags(Qt::Widget);
    dialog_->setModal(false);
    dialog_->setMinimumSize(0, 0);

    auto* publicNavigation = dialog_->findChild<QListWidget*>(QStringLiteral("settingsNavigation"));
    auto* publicPages = dialog_->findChild<QStackedWidget*>(QStringLiteral("settingsPages"));
    auto* themeList = dialog_->findChild<QListWidget*>(QStringLiteral("installedThemeList"));
    auto* followSystem = dialog_->findChild<QCheckBox*>(QStringLiteral("followSystemTheme"));

    auto* body = new QWidget(content);
    body->setObjectName(QStringLiteral("settingsWorkspaceBody"));
    auto* bodyLayout = new QHBoxLayout(body);
    chrome_geometry::margins(bodyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(bodyLayout, 35);
    auto* navigation = new QListWidget(body);
    navigation_ = navigation;
    navigation->setObjectName(QStringLiteral("settingsWorkspaceNavigation"));
    chrome_geometry::fixedWidth(navigation, 173);
    chrome_geometry::iconSize(navigation, QSize(14, 14));
    chrome_geometry::spacing(navigation, 2);
    navigation->setWordWrap(true);
    auto addNavigation = [navigation](const QString& title, const QString& id) {
        auto* item = new QListWidgetItem(title, navigation);
        item->setData(Qt::UserRole, id);
        item->setData(Qt::UserRole + 1, navigationIconBody(id));
        item->setIcon(navigationIcon(navigationIconBody(id), QColor(QStringLiteral("#7A7E86")),
                                     navigation->iconSize()));
        item->setSizeHint(QSize(0, id == QStringLiteral("account-devices") ? 48 : 34));
    };
    addNavigation(tr("Appearance"), QStringLiteral("appearance"));
    addNavigation(tr("Typography"), QStringLiteral("typography"));
    addNavigation(tr("Account & devices"), QStringLiteral("account-devices"));
    addNavigation(tr("Agent models"), QStringLiteral("agent-models"));
    addNavigation(tr("Browser"), QStringLiteral("browser"));
    addNavigation(tr("Computer use"), QStringLiteral("computer"));
    addNavigation(tr("Providers"), QStringLiteral("providers"));

    auto* pages = new QStackedWidget(body);
    pages->setObjectName(QStringLiteral("settingsWorkspacePages"));

    auto* appearance = new QWidget(pages);
    appearance->setObjectName(QStringLiteral("settingsWorkspaceAppearance"));
    auto* appearanceLayout = new QVBoxLayout(appearance);
    chrome_geometry::margins(appearanceLayout, 0, 0, 8, 24);
    chrome_geometry::spacing(appearanceLayout, 0);
    appearanceLayout->addWidget(
        label(tr("Appearance"), QStringLiteral("settingsWorkspaceSectionTitle"), appearance));
    chrome_geometry::addSpacing(appearanceLayout, 7);
    appearanceLayout->addWidget(
        label(tr("Choose a theme that suits your lighting and helps you focus."),
              QStringLiteral("settingsWorkspaceIntro"), appearance));
    chrome_geometry::addSpacing(appearanceLayout, 24);

    auto* themes = new QWidget(appearance);
    auto* themesLayout = new QHBoxLayout(themes);
    chrome_geometry::margins(themesLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(themesLayout, 14);
    auto* lightCard = themeCard(tr("Light"), false, themes);
    auto* darkCard = themeCard(tr("Dark"), true, themes);
    auto* themeGroup = new QButtonGroup(themes);
    themeGroup->setExclusive(true);
    themeGroup->addButton(lightCard, static_cast<int>(ThemeManager::Mode::Light));
    themeGroup->addButton(darkCard, static_cast<int>(ThemeManager::Mode::Dark));
    themesLayout->addWidget(lightCard);
    themesLayout->addWidget(darkCard);
    themesLayout->addStretch(1);
    appearanceLayout->addWidget(themes);
    chrome_geometry::addSpacing(appearanceLayout, 14);

    if (followSystem != nullptr) {
        followSystem->setParent(appearance);
        appearanceLayout->addWidget(followSystem);
    }
    chrome_geometry::addSpacing(appearanceLayout, 21);
    appearanceLayout->addWidget(divider(appearance));
    chrome_geometry::addSpacing(appearanceLayout, 20);
    appearanceLayout->addWidget(
        label(tr("Accent color"), QStringLiteral("settingsWorkspaceSubheading"), appearance));
    chrome_geometry::addSpacing(appearanceLayout, 10);
    auto* accent = new QFrame(appearance);
    accent->setObjectName(QStringLiteral("settingsWorkspaceAccent"));
    auto* accentLayout = new QHBoxLayout(accent);
    chrome_geometry::margins(accentLayout, 13, 10, 13, 10);
    chrome_geometry::spacing(accentLayout, 10);
    auto* accentSwatch = label(QString(), QStringLiteral("settingsAccentSwatch"), accent);
    chrome_geometry::fixedSize(accentSwatch, 29, 29);
    accentLayout->addWidget(accentSwatch);
    auto* accentCopy = new QWidget(accent);
    auto* accentCopyLayout = new QVBoxLayout(accentCopy);
    chrome_geometry::margins(accentCopyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(accentCopyLayout, 2);
    accentCopyLayout->addWidget(
        label(tr("Indigo"), QStringLiteral("settingsAccentTitle"), accentCopy));
    accentCopyLayout->addWidget(label(tr("The signature color of Két Design System"),
                                      QStringLiteral("settingsWorkspaceHelp"), accentCopy));
    accentLayout->addWidget(accentCopy, 1);
    accentLayout->addWidget(
        label(QStringLiteral("✓"), QStringLiteral("settingsAccentCheck"), accent));
    appearanceLayout->addWidget(accent);
    chrome_geometry::addSpacing(appearanceLayout, 21);
    appearanceLayout->addWidget(divider(appearance));
    chrome_geometry::addSpacing(appearanceLayout, 20);
    appearanceLayout->addWidget(
        label(tr("Workspace preview"), QStringLiteral("settingsWorkspaceSubheading"), appearance));
    chrome_geometry::addSpacing(appearanceLayout, 10);
    auto* preview = new QFrame(appearance);
    preview->setObjectName(QStringLiteral("settingsWorkspacePreview"));
    auto* previewLayout = new QVBoxLayout(preview);
    chrome_geometry::margins(previewLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(previewLayout, 0);
    previewLayout->addWidget(label(QStringLiteral("  ◇  NavigationRail.qml     README.md"),
                                   QStringLiteral("settingsPreviewTabs"), preview));
    previewLayout->addWidget(label(QStringLiteral("// Focus on what you are creating.\n"
                                                  "property string workspace:  \"develop\"\n"
                                                  "color: theme.accentSubtle"),
                                   QStringLiteral("settingsPreviewCode"), preview));
    previewLayout->addWidget(
        label(QStringLiteral("◆  Ready for your next idea.                         Completed"),
              QStringLiteral("settingsPreviewMessage"), preview));
    appearanceLayout->addWidget(preview);
    chrome_geometry::addSpacing(appearanceLayout, 8);
    appearanceLayout->addWidget(
        label(tr("Applies to the interface, editor, diffs, Markdown, and terminal."),
              QStringLiteral("settingsWorkspaceHelp"), appearance));
    appearanceLayout->addStretch(1);
    pages->addWidget(scrollPage(appearance, pages));

    auto* typography = new QWidget(pages);
    typography->setObjectName(QStringLiteral("settingsWorkspaceTypography"));
    auto* typographyLayout = new QVBoxLayout(typography);
    chrome_geometry::margins(typographyLayout, 0, 0, 8, 24);
    chrome_geometry::spacing(typographyLayout, 0);
    typographyLayout->addWidget(
        label(tr("Typography"), QStringLiteral("settingsWorkspaceSectionTitle"), typography));
    chrome_geometry::addSpacing(typographyLayout, 7);
    typographyLayout->addWidget(label(tr("Adjust readability across your workspace."),
                                      QStringLiteral("settingsWorkspaceIntro"), typography));
    chrome_geometry::addSpacing(typographyLayout, 25);
    auto* fields = new QWidget(typography);
    auto* fieldsLayout = new QGridLayout(fields);
    chrome_geometry::margins(fieldsLayout, 0, 0, 0, 0);
    chrome_geometry::horizontalSpacing(fieldsLayout, 20);
    chrome_geometry::verticalSpacing(fieldsLayout, 13);
    fieldsLayout->setColumnMinimumWidth(0, 145);
    fieldsLayout->setColumnStretch(1, 1);
    auto* editorFont = dialog_->findChild<QComboBox*>(QStringLiteral("editorFontFamily"));
    auto* interfaceSize = dialog_->findChild<QSpinBox*>(QStringLiteral("interfaceFontSize"));
    auto* editorSize = dialog_->findChild<QSpinBox*>(QStringLiteral("editorFontSize"));
    auto* editorLine = dialog_->findChild<QSpinBox*>(QStringLiteral("editorLineHeight"));
    auto* terminalSize = dialog_->findChild<QSpinBox*>(QStringLiteral("terminalFontSize"));
    auto* terminalLine = dialog_->findChild<QSpinBox*>(QStringLiteral("terminalLineHeight"));
    auto* previewSize = dialog_->findChild<QSpinBox*>(QStringLiteral("previewFontSize"));
    auto* previewLine = dialog_->findChild<QSpinBox*>(QStringLiteral("previewLineHeight"));
    const QList<QWidget*> publicControls = {editorFont,   interfaceSize, editorSize,  editorLine,
                                            terminalSize, terminalLine,  previewSize, previewLine};
    for (QWidget* control : publicControls) {
        if (control != nullptr) {
            control->setParent(fields);
            control->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            control->setMaximumWidth(QWIDGETSIZE_MAX);
        }
    }
    if (editorLine != nullptr)
        editorLine->hide();
    if (terminalLine != nullptr)
        terminalLine->hide();
    if (previewLine != nullptr)
        previewLine->hide();
    if (interfaceSize != nullptr)
        interfaceSize->hide();
    auto* interfaceFont = new QComboBox(fields);
    interfaceFont->setObjectName(QStringLiteral("settingsInterfaceFont"));
    interfaceFont->addItem(InterfaceUiFont::settingsLabel());
    interfaceFont->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* interfaceZoom = segmentedGroup(QStringLiteral("interfaceZoom"), fields);
    auto* interfaceZoomLayout = static_cast<QHBoxLayout*>(interfaceZoom->layout());
    auto* zoomGroup = new QButtonGroup(interfaceZoom);
    zoomGroup->setExclusive(true);
    auto addZoomButton = [&](const InterfaceZoom zoom, const QString& objectName,
                             const QString& text) {
        auto* button = new QToolButton(interfaceZoom);
        button->setObjectName(objectName);
        button->setProperty("kvRole", QStringLiteral("interfaceZoom"));
        button->setText(text);
        button->setCheckable(true);
        button->setAutoExclusive(true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        zoomGroup->addButton(button);
        interfaceZoomLayout->addWidget(button);
        if (interfaceSize != nullptr) {
            connect(button, &QToolButton::clicked, this, [interfaceSize, zoom] {
                interfaceSize->setValue(InterfaceZoomMetrics::pixels(zoom));
            });
        }
        return button;
    };
    auto* zoomSmall =
        addZoomButton(InterfaceZoom::Small, QStringLiteral("interfaceZoomSmall"), tr("Small"));
    auto* zoomMedium = addZoomButton(InterfaceZoom::Medium, QStringLiteral("interfaceZoomMedium"),
                                     tr("Medium (Default)"));
    auto* zoomLarge =
        addZoomButton(InterfaceZoom::Large, QStringLiteral("interfaceZoomLarge"), tr("Large"));
    const auto syncZoomButtons = [interfaceSize, zoomSmall, zoomMedium, zoomLarge] {
        const auto zoom = InterfaceZoomMetrics::fromPixels(
            interfaceSize != nullptr ? interfaceSize->value() : InterfaceZoomMetrics::mediumPixels);
        zoomSmall->setChecked(zoom == InterfaceZoom::Small);
        zoomMedium->setChecked(zoom == InterfaceZoom::Medium);
        zoomLarge->setChecked(zoom == InterfaceZoom::Large);
    };
    if (interfaceSize != nullptr) {
        connect(interfaceSize, &QSpinBox::valueChanged, this,
                [this, syncZoomButtons](const int pixels) {
                    syncZoomButtons();
                    if (palette_.pageBackground.isEmpty())
                        return;
                    applyPalette(palette_, pixels);
                });
    }
    syncZoomButtons();
    auto* interfaceSizeCopy = new QWidget(fields);
    auto* interfaceSizeCopyLayout = new QVBoxLayout(interfaceSizeCopy);
    chrome_geometry::margins(interfaceSizeCopyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(interfaceSizeCopyLayout, 2);
    interfaceSizeCopyLayout->addWidget(label(
        tr("Interface size"), QStringLiteral("settingsWorkspaceFieldLabel"), interfaceSizeCopy));
    interfaceSizeCopyLayout->addWidget(
        label(tr("Makes chrome, sidebar, and dialogs larger or smaller."),
              QStringLiteral("settingsWorkspaceHelp"), interfaceSizeCopy));
    addField(fieldsLayout, 0, tr("Interface font"), interfaceFont, fields);
    fieldsLayout->addWidget(interfaceSizeCopy, 1, 0, Qt::AlignTop);
    fieldsLayout->addWidget(interfaceZoom, 1, 1);
    addField(fieldsLayout, 2, tr("Editor font"), editorFont, fields);
    addField(fieldsLayout, 3, tr("Editor font size"), editorSize, fields);
    addField(fieldsLayout, 4, tr("Terminal font size"), terminalSize, fields);
    addField(fieldsLayout, 5, tr("Markdown font size"), previewSize, fields);
    // A segmented group like Interface size, one button per width. The choice is
    // staged until Save, like the fields above.
    auto* widthState = new ConversationWidthState;
    dialog_->addPage(widthState);
    auto* conversationWidth = segmentedGroup(QStringLiteral("agentConversationWidth"), fields);
    auto* conversationWidthLayout = static_cast<QHBoxLayout*>(conversationWidth->layout());
    auto* widthGroup = new QButtonGroup(conversationWidth);
    widthGroup->setExclusive(true);
    for (const auto& choice : AgentConversationWidth::choices()) {
        auto* button = new QToolButton(conversationWidth);
        button->setObjectName(QStringLiteral("agentConversationWidth%1").arg(choice.pixels));
        button->setProperty("kvRole", QStringLiteral("interfaceZoom"));
        button->setProperty("conversationWidth", choice.pixels);
        button->setText(choice.label);
        if (choice.pixels > 0)
            button->setToolTip(tr("%1 px").arg(choice.pixels));
        button->setCheckable(true);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        widthGroup->addButton(button);
        conversationWidthLayout->addWidget(button);
        connect(button, &QToolButton::clicked, widthState,
                [widthState, pixels = choice.pixels] { widthState->select(pixels); });
    }
    const auto syncWidthButtons = [widthGroup, widthState] {
        for (auto* button : widthGroup->buttons())
            button->setChecked(button->property("conversationWidth").toInt() ==
                               widthState->pixels());
    };
    // Reset defaults changes the state without a click.
    connect(widthState, &SettingsPage::changed, conversationWidth, syncWidthButtons);
    syncWidthButtons();
    auto* conversationWidthCopy = new QWidget(fields);
    auto* conversationWidthCopyLayout = new QVBoxLayout(conversationWidthCopy);
    chrome_geometry::margins(conversationWidthCopyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(conversationWidthCopyLayout, 2);
    conversationWidthCopyLayout->addWidget(label(tr("Conversation width"),
                                                 QStringLiteral("settingsWorkspaceFieldLabel"),
                                                 conversationWidthCopy));
    conversationWidthCopyLayout->addWidget(
        label(tr("How wide AI workspace conversations can grow. Atlas keeps its own layout."),
              QStringLiteral("settingsWorkspaceHelp"), conversationWidthCopy));
    fieldsLayout->addWidget(conversationWidthCopy, 6, 0, Qt::AlignTop);
    fieldsLayout->addWidget(conversationWidth, 6, 1);
    typographyLayout->addWidget(fields);
    chrome_geometry::addSpacing(typographyLayout, 24);
    auto* sample = new QFrame(typography);
    sample->setObjectName(QStringLiteral("settingsTypographySample"));
    auto* sampleLayout = new QVBoxLayout(sample);
    chrome_geometry::margins(sampleLayout, 20, 18, 20, 18);
    chrome_geometry::spacing(sampleLayout, 10);
    sampleLayout->addWidget(
        label(tr("PREVIEW"), QStringLiteral("settingsWorkspaceEyebrow"), sample));
    sampleLayout->addWidget(label(tr("A space for what you are creating."),
                                  QStringLiteral("settingsTypographyUiSample"), sample));
    sampleLayout->addWidget(label(QStringLiteral("const workspace = \"develop\";\n"
                                                 "agent.continue(workspace);"),
                                  QStringLiteral("settingsTypographyCodeSample"), sample));
    sampleLayout->addWidget(label(QStringLiteral("➜ ketplus-desktop git:(develop)"),
                                  QStringLiteral("settingsTypographyTerminalSample"), sample));
    sampleLayout->addWidget(label(tr("Good ideas start with a clear workspace."),
                                  QStringLiteral("settingsTypographyMarkdownSample"), sample));
    typographyLayout->addWidget(sample);
    typographyLayout->addStretch(1);
    pages->addWidget(scrollPage(typography, pages));
    pages->addWidget(
        scrollPage(new SettingsBrowserPage({}, {}, browser_tools::systemProbes(), pages), pages));
    pages->addWidget(scrollPage(
        new SettingsProvidersPage(std::move(providers), pages, std::move(credentials)), pages));

    if (publicNavigation != nullptr && publicPages != nullptr) {
        for (int index = 0; index < publicNavigation->count(); ++index) {
            if (publicNavigation->item(index)->data(Qt::UserRole).toString() !=
                QStringLiteral("archived"))
                continue;
            QWidget* archived = publicPages->widget(index);
            publicPages->removeWidget(archived);
            archived->setParent(pages);
            addNavigation(tr("Archived"), QStringLiteral("archived"));
            pages->addWidget(archived);
            break;
        }
    }
    auto* storage = scrollPage(new SettingsStoragePage(std::move(storageRequester), pages), pages);
    addNavigation(tr("Storage"), QStringLiteral("storage"));
    pages->addWidget(storage);
    auto* updates = scrollPage(new SettingsUpdatesPage(AppUpdater::instance(), pages), pages);
    addNavigation(tr("Updates"), QStringLiteral("updates"));
    pages->addWidget(updates);
    // Navigated by widget: the numbered pages above keep their indices.
    auto* computer = scrollPage(new SettingsComputerPage({}, {}, pages), pages);
    pages->addWidget(computer);

    bodyLayout->addWidget(navigation);
    bodyLayout->addWidget(pages, 1);
    contentLayout->addWidget(body, 1);

    auto* settingsFooter = dialog_->findChild<QDialogButtonBox*>();
    if (auto* buttons = settingsFooter) {
        if (auto* cancel = buttons->button(QDialogButtonBox::Cancel))
            cancel->hide();
        buttons->setParent(content);
        buttons->setObjectName(QStringLiteral("settingsWorkspaceFooter"));
        contentLayout->addWidget(buttons);
    }

    auto selectPublicTheme = [themeList, followSystem](const int mode) {
        if (themeList == nullptr)
            return;
        if (followSystem != nullptr)
            followSystem->setChecked(false);
        for (int index = 0; index < themeList->count(); ++index) {
            if (themeList->item(index)->data(Qt::UserRole).toInt() == mode) {
                themeList->setCurrentRow(index);
                return;
            }
        }
    };
    connect(lightCard, &QToolButton::clicked, this, [selectPublicTheme] {
        selectPublicTheme(static_cast<int>(ThemeManager::Mode::Light));
    });
    connect(darkCard, &QToolButton::clicked, this,
            [selectPublicTheme] { selectPublicTheme(static_cast<int>(ThemeManager::Mode::Dark)); });
    auto syncThemeCards = [themeList, followSystem, lightCard, darkCard] {
        const bool system = followSystem != nullptr && followSystem->isChecked();
        const int mode = themeList == nullptr || themeList->currentItem() == nullptr
                             ? static_cast<int>(ThemeManager::Mode::System)
                             : themeList->currentItem()->data(Qt::UserRole).toInt();
        lightCard->setChecked(!system && mode == static_cast<int>(ThemeManager::Mode::Light));
        darkCard->setChecked(!system && mode == static_cast<int>(ThemeManager::Mode::Dark));
    };
    if (themeList != nullptr)
        connect(themeList, &QListWidget::currentItemChanged, this,
                [syncThemeCards](QListWidgetItem*, QListWidgetItem*) { syncThemeCards(); });
    if (followSystem != nullptr)
        connect(followSystem, &QCheckBox::toggled, this,
                [syncThemeCards](bool) { syncThemeCards(); });
    syncThemeCards();

    connect(navigation, &QListWidget::currentRowChanged, this,
            [this, navigation, pages, updates, computer, storage, settingsFooter](const int row) {
                for (int index = 0; index < navigation->count(); ++index) {
                    const bool dark =
                        navigation->palette().color(QPalette::Window).lightness() < 128;
                    // Két Design System accent-text: Két Blue that stays readable on dark.
                    const QColor color(index == row ? (dark ? QStringLiteral("#9EB5FF")
                                                            : QStringLiteral("#1238DB"))
                                                    : QStringLiteral("#7A7E86"));
                    navigation->item(index)->setIcon(navigationIcon(
                        navigation->item(index)->data(Qt::UserRole + 1).toByteArray(), color,
                        navigation->iconSize()));
                }
                const QString id =
                    row < 0 ? QString() : navigation->item(row)->data(Qt::UserRole).toString();
                // Storage changes save immediately; global appearance reset/save do not apply.
                if (settingsFooter)
                    settingsFooter->setVisible(id != QStringLiteral("storage"));
                if (id == QStringLiteral("appearance")) {
                    pages->setCurrentIndex(0);
                } else if (id == QStringLiteral("typography")) {
                    pages->setCurrentIndex(1);
                } else if (id == QStringLiteral("account-devices")) {
                    emit accountDevicesRequested();
                } else if (id == QStringLiteral("agent-models")) {
                    emit agentModelsRequested();
                } else if (id == QStringLiteral("browser")) {
                    pages->setCurrentIndex(2);
                } else if (id == QStringLiteral("computer")) {
                    pages->setCurrentWidget(computer);
                } else if (id == QStringLiteral("providers")) {
                    pages->setCurrentIndex(3);
                } else if (id == QStringLiteral("archived")) {
                    pages->setCurrentIndex(4);
                } else if (id == QStringLiteral("storage")) {
                    pages->setCurrentWidget(storage);
                } else if (id == QStringLiteral("updates")) {
                    pages->setCurrentWidget(updates);
                }
            });
    navigation->setCurrentRow(0);
    QTimer::singleShot(0, navigation, [navigation] { navigation->setCurrentRow(0); });
    connect(dialog_, &QDialog::accepted, this, &SettingsWorkspacePage::saved);
    connect(dialog_, &QDialog::rejected, this, &SettingsWorkspacePage::dismissed);
    dialog_->hide();
    layout->addWidget(content, 1, Qt::AlignHCenter);
}

SettingsDialog* SettingsWorkspacePage::dialog() const noexcept { return dialog_; }

void SettingsWorkspacePage::showSection(const QString& id) {
    for (int row = 0; row < navigation_->count(); ++row) {
        if (navigation_->item(row)->data(Qt::UserRole).toString() == id) {
            navigation_->setCurrentRow(row);
            // The constructor's deferred reset to the first row must not undo this request.
            QTimer::singleShot(0, navigation_,
                               [navigation = navigation_, row] { navigation->setCurrentRow(row); });
            return;
        }
    }
}

void SettingsWorkspacePage::applyPalette(const ThemePalette& palette,
                                         const int interfaceFontSizePixels) {
    palette_ = palette;
    interfaceFontSizePixels_ = InterfaceZoomMetrics::snap(interfaceFontSizePixels);
    const QString sheet = TypeScale::apply(QStringLiteral(R"QSS(
QWidget#settingsWorkspacePage, QWidget#settingsWorkspaceContent,
QWidget#settingsWorkspaceHeading, QWidget#settingsWorkspaceBody,
QWidget#settingsWorkspaceAppearance, QWidget#settingsWorkspaceTypography,
QStackedWidget#settingsWorkspacePages, QScrollArea#settingsWorkspaceScroll,
QScrollArea#settingsWorkspaceScroll > QWidget > QWidget { background:%1; color:%2; }
QLabel#settingsWorkspaceEyebrow { color:%9; font-size:{{caption}}px; font-weight:600; letter-spacing:1px; }
QLabel#settingsWorkspaceTitle { color:%2; font-size:{{display}}px; font-weight:620; }
QLabel#settingsWorkspaceSubtitle, QLabel#settingsWorkspaceIntro { color:%3; font-size:{{reading}}px; }
QLabel#settingsWorkspaceShortcut { color:%3; font-size:{{caption}}px; }
QLabel#settingsWorkspaceHelp { color:%3; font-size:{{body}}px; }
QLabel#settingsWorkspaceSectionTitle { color:%2; font-size:{{heading}}px; font-weight:600; }
QLabel#settingsWorkspaceSubheading { color:%2; font-size:{{reading}}px; font-weight:600; }
QListWidget#settingsWorkspaceNavigation { padding:0 20px 0 0; border:0;
 border-right:1px solid %5; background:transparent; outline:0; }
QListWidget#settingsWorkspaceNavigation::item { height:34px; padding:0 10px; border-radius:5px;
 color:%3; font-size:{{reading}}px; }
QListWidget#settingsWorkspaceNavigation::item:selected { background:%6; color:%9; }
QToolButton[kvRole="settingsThemeCard"] { padding:7px; border:1px solid %5; border-radius:7px;
 background:%7; color:%2; font-size:{{reading}}px; font-weight:500; }
QToolButton[kvRole="settingsThemeCard"]:hover { border-color:%8; }
QToolButton[kvRole="settingsThemeCard"]:checked { border:2px solid %4; background:%6; }
QWidget[kvRole="segmentedGroup"] { border:1px solid %5; border-radius:8px; background:%7; }
QToolButton[kvRole="interfaceZoom"] { min-height:24px; padding:0 12px; border:1px solid transparent;
 border-radius:5px; background:transparent; color:%2; font-size:{{reading}}px; font-weight:500; }
QToolButton[kvRole="interfaceZoom"]:hover { background:%1; }
QToolButton[kvRole="interfaceZoom"]:checked { background:%6; color:%9; border-color:%4; }
QFrame#settingsWorkspaceDivider { border:0; background:%5; }
QFrame#settingsWorkspaceAccent { border:0; background:transparent; }
QFrame#settingsWorkspacePreview, QFrame#settingsTypographySample {
 border:1px solid %5; border-radius:7px; background:%7; }
QLabel#settingsAccentSwatch { border:3px solid %6; border-radius:6px; background:%4; }
QLabel#settingsAccentCheck { color:%9; font-size:{{reading}}px; }
QLabel#settingsAccentTitle { color:%2; font-size:{{body}}px; font-weight:600; }
QLabel#settingsPreviewTabs { min-height:32px; padding:0 12px; border-bottom:2px solid %4;
 color:%2; background:%7; font-size:{{micro}}px; }
QLabel#settingsPreviewCode { padding:14px 17px; color:%3; font-family:monospace; font-size:{{body}}px; }
QLabel#settingsPreviewMessage { min-height:34px; padding:0 17px; border-top:1px solid %5;
 color:%2; background:%7; font-size:{{body}}px; }
QCheckBox#followSystemTheme, QCheckBox#storageAutomaticDetail, QLabel#storageUsage,
QLabel#storageStatus, QComboBox#storageDetailDays, QLabel#settingsWorkspaceFieldLabel,
QComboBox#settingsInterfaceFont, QComboBox#editorFontFamily, QComboBox#computerWorkDisplay,
QSpinBox#interfaceFontSize, QSpinBox#editorFontSize, QSpinBox#terminalFontSize,
QSpinBox#previewFontSize { color:%2; font-size:{{reading}}px; }
QLabel#settingsTypographyUiSample { color:%2; font-size:{{reading}}px; }
QLabel#settingsTypographyCodeSample, QLabel#settingsTypographyTerminalSample,
QLabel#settingsTypographyMarkdownSample {
 color:%3; font-family:monospace; font-size:{{reading}}px; }
QDialogButtonBox#settingsWorkspaceFooter { padding-top:18px; border-top:1px solid %5;
 background:transparent; }
QDialog[kvRole="settingsDialog"] { border:0; background:transparent; }
)QSS"),
                                           interfaceFontSizePixels_);
    setStyleSheet(sheet.arg(palette.pageBackground, palette.textMain, palette.textMuted,
                            palette.accent, palette.border, palette.accentSubtle,
                            palette.panelBackground, palette.borderStrong, palette.accentText));
    chrome_geometry::apply(this, interfaceFontSizePixels);
}

} // namespace ketplus

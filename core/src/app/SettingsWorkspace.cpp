#include "app/SettingsWorkspace.h"

#include "KetAppearance.h"
#include "KetIcons.h"
#include "KetNavItem.h"
#include "KetSegmentedControl.h"
#include "KetThemeCard.h"
#include "KetTokens.h"
#include "app/PixelSelect.h"
#include "app/SettingsAppearancePreview.h"
#include "app/SettingsDialog.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "ui/TypeScale.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPersistentModelIndex>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "KetSelect.h"

namespace ketplus {
namespace {

// The mock's 1220 px reading width.
class SettingsPageContent final : public QWidget {
  public:
    using QWidget::QWidget;

    QSize sizeHint() const override {
        QSize hint = QWidget::sizeHint();
        hint.setWidth(1220);
        return hint;
    }
};

QString navigationIcon(const QString& id) {
    static const QHash<QString, QString> names{
        {QStringLiteral("appearance"), QStringLiteral("palette")},
        {QStringLiteral("typography"), QStringLiteral("type")},
        {QStringLiteral("account-devices"), QStringLiteral("monitor")},
        {QStringLiteral("providers"), QStringLiteral("package")},
        {QStringLiteral("computer"), QStringLiteral("monitor")},
        {QStringLiteral("browser"), QStringLiteral("globe")},
        {QStringLiteral("storage"), QStringLiteral("layers")},
        {QStringLiteral("updates"), QStringLiteral("download")},
        {QStringLiteral("agent-models"), QStringLiteral("sliders-horizontal")},
        {QStringLiteral("teams"), QStringLiteral("bot")},
    };
    return ket::Icons::codeForLucide(names.value(id, QStringLiteral("archive")));
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

// The mock's 200 px select at the end of its row.
void fitSelect(QWidget* control) {
    control->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    chrome_geometry::fixedWidth(control, 200);
}

// A section's title and intro, as each mock page starts.
QVBoxLayout* sectionLayout(QWidget* page, const QString& title, const QString& intro) {
    auto* layout = new QVBoxLayout(page);
    chrome_geometry::margins(layout, 0, 0, 8, 24);
    chrome_geometry::spacing(layout, 0);
    layout->addWidget(
        SettingsWorkspace::label(title, QStringLiteral("settingsWorkspaceSectionTitle"), page));
    chrome_geometry::addSpacing(layout, 7);
    layout->addWidget(
        SettingsWorkspace::label(intro, QStringLiteral("settingsWorkspaceIntro"), page));
    return layout;
}

void addSubheading(QVBoxLayout* layout, const QString& text, QWidget* parent) {
    layout->addWidget(
        SettingsWorkspace::label(text, QStringLiteral("settingsWorkspaceSubheading"), parent));
    chrome_geometry::addSpacing(layout, 10);
}

void addDivider(QVBoxLayout* layout, QWidget* parent) {
    chrome_geometry::addSpacing(layout, 21);
    layout->addWidget(divider(parent));
    chrome_geometry::addSpacing(layout, 20);
}

} // namespace

QLabel* SettingsWorkspace::label(const QString& text, const QString& objectName, QWidget* parent) {
    auto* value = new QLabel(text, parent);
    value->setObjectName(objectName);
    value->setWordWrap(true);
    return value;
}

SettingsWorkspace::SettingsWorkspace(SettingsDialog* dialog, const QString& shortcut,
                                     const QString& interfaceFont, QWidget* parent)
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
    auto* keys = new ket::Kbd(shortcut, heading);
    keys->setObjectName(QStringLiteral("settingsWorkspaceShortcut"));
    headingLayout->addWidget(keys, 0, Qt::AlignTop);
    contentLayout->addWidget(heading);
    chrome_geometry::addSpacing(contentLayout, 28);

    // The dialog keeps the settings and their drafts; its controls move onto this page.
    dialog_->setParent(this);
    dialog_->setWindowFlags(Qt::Widget);
    dialog_->setModal(false);
    dialog_->setMinimumSize(0, 0);

    auto* body = new QWidget(content);
    body->setObjectName(QStringLiteral("settingsWorkspaceBody"));
    auto* bodyLayout = new QHBoxLayout(body);
    chrome_geometry::margins(bodyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(bodyLayout, 35);
    navigation_ = new QListWidget(body);
    navigation_->setObjectName(QStringLiteral("settingsWorkspaceNavigation"));
    chrome_geometry::fixedWidth(navigation_, 200);
    chrome_geometry::iconSize(navigation_, QSize(14, 14));
    chrome_geometry::spacing(navigation_, 2);
    navigation_->setWordWrap(true);
    pages_ = new QStackedWidget(body);
    pages_->setObjectName(QStringLiteral("settingsWorkspacePages"));
    bodyLayout->addWidget(navigation_);
    bodyLayout->addWidget(pages_, 1);
    contentLayout->addWidget(body, 1);

    addSection(tr("Appearance"), QStringLiteral("appearance"), createAppearance());
    addSection(tr("Typography"), QStringLiteral("typography"), createTypography(interfaceFont));

    // The dialog's footer holds Restore defaults; every change applies as it is made.
    footer_ = dialog_->findChild<QWidget*>(QStringLiteral("settingsButtons"));
    if (footer_ != nullptr) {
        footer_->setParent(content);
        footer_->setObjectName(QStringLiteral("settingsWorkspaceFooter"));
        contentLayout->addWidget(footer_);
        footer_->show();
    }

    connect(navigation_, &QListWidget::currentRowChanged, this, &SettingsWorkspace::showRow);
    navigation_->setCurrentRow(0);
    connect(dialog_, &QDialog::accepted, this, &SettingsWorkspace::saved);
    connect(dialog_, &QDialog::rejected, this, &SettingsWorkspace::dismissed);
    dialog_->hide();
    layout->addWidget(content, 1, Qt::AlignHCenter);

    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            &SettingsWorkspace::applyAppearance);
    if (auto* theme = dialog_->theme())
        connect(theme, &ThemeManager::themeChanged, this, &SettingsWorkspace::applyAppearance);
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this,
            &SettingsWorkspace::applyAppearance);
    applyAppearance();
}

QWidget* SettingsWorkspace::createAppearance() {
    auto* appearance = new QWidget(pages_);
    appearance->setObjectName(QStringLiteral("settingsWorkspaceAppearance"));
    auto* layout =
        sectionLayout(appearance, tr("Appearance"),
                      tr("Choose a theme that suits your lighting and helps you focus."));
    chrome_geometry::addSpacing(layout, 24);

    auto* themes = new QWidget(appearance);
    auto* themesLayout = new QHBoxLayout(themes);
    chrome_geometry::margins(themesLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(themesLayout, 14);
    for (const auto* name : {"lightThemeCard", "darkThemeCard"}) {
        if (auto* card = dialog_->findChild<ket::ThemeCard*>(QLatin1String(name))) {
            card->setParent(themes);
            chrome_geometry::fixedWidth(card, 264);
            themesLayout->addWidget(card);
            card->show();
        }
    }
    themesLayout->addStretch(1);
    layout->addWidget(themes);
    chrome_geometry::addSpacing(layout, 14);
    if (auto* followSystem =
            dialog_->findChild<QAbstractButton*>(QStringLiteral("followSystemTheme"))) {
        followSystem->setParent(appearance);
        layout->addWidget(followSystem);
    }
    addDivider(layout, appearance);

    addSubheading(layout, tr("Accent color"), appearance);
    auto* accent = new QFrame(appearance);
    accent->setObjectName(QStringLiteral("settingsWorkspaceAccent"));
    auto* accentLayout = new QHBoxLayout(accent);
    chrome_geometry::margins(accentLayout, 13, 10, 13, 10);
    chrome_geometry::spacing(accentLayout, 10);
    auto* swatch = new ket::AccentSwatch(accent);
    swatch->setObjectName(QStringLiteral("settingsAccentSwatch"));
    accentLayout->addWidget(swatch);
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
    layout->addWidget(accent);
    addDivider(layout, appearance);

    // The mock's zoom steps, from the dialog; they scale the chrome (ADR 0104).
    if (auto* zoom = dialog_->findChild<ket::SegmentedControl*>(QStringLiteral("interfaceZoom"))) {
        addSubheading(layout, tr("Interface zoom"), appearance);
        zoom->setParent(appearance);
        layout->addWidget(zoom, 0, Qt::AlignLeft);
        addDivider(layout, appearance);
    }

    addSubheading(layout, tr("Workspace preview"), appearance);
    auto* preview = new WorkspacePreview(appearance);
    // The code reads in the editor font, KÉT Sans Mono by default, as it will in the editor.
    const auto showEditorFont = [preview, dialog = dialog_] {
        preview->setCodeFamily(dialog->settings().fontFamily);
    };
    showEditorFont();
    if (auto* editorFamily = dialog_->findChild<QComboBox*>(QStringLiteral("editorFontFamily")))
        connect(editorFamily, &QComboBox::currentIndexChanged, preview, showEditorFont);
    layout->addWidget(preview);
    chrome_geometry::addSpacing(layout, 8);
    layout->addWidget(label(tr("Applies to the interface, editor, diffs, Markdown, and terminal."),
                            QStringLiteral("settingsWorkspaceHelp"), appearance));
    layout->addStretch(1);
    return appearance;
}

QWidget* SettingsWorkspace::createTypography(const QString& interfaceFont) {
    auto* typography = new QWidget(pages_);
    typography->setObjectName(QStringLiteral("settingsWorkspaceTypography"));
    auto* layout = sectionLayout(typography, tr("Typography"),
                                 tr("Adjust readability across your workspace."));
    chrome_geometry::addSpacing(layout, 25);
    auto* fields = new QWidget(typography);
    typographyFields_ = new QGridLayout(fields);
    chrome_geometry::margins(typographyFields_, 0, 0, 0, 0);
    chrome_geometry::horizontalSpacing(typographyFields_, 20);
    chrome_geometry::verticalSpacing(typographyFields_, 13);
    typographyFields_->setColumnMinimumWidth(0, 145);
    // The labels take the row, so their help reads on one line as in the mock.
    typographyFields_->setColumnStretch(0, 1);

    auto* editorFont = dialog_->findChild<QComboBox*>(QStringLiteral("editorFontFamily"));
    auto* interfaceSize = dialog_->findChild<PixelSelect*>(QStringLiteral("interfaceFontSize"));
    auto* editorSize = dialog_->findChild<PixelSelect*>(QStringLiteral("editorFontSize"));
    // The mock has no line height row; the editor keeps its saved line height.
    if (auto* editorLine = dialog_->findChild<PixelSelect*>(QStringLiteral("editorLineHeight"))) {
        editorLine->setParent(fields);
        editorLine->hide();
    }
    auto* font = new ket::Select(fields);
    font->setObjectName(QStringLiteral("settingsInterfaceFont"));
    font->addItem(interfaceFont);
    font->setAccessibleName(tr("Interface font"));
    addTypographyRow(0, tr("Interface font"),
                     tr("KetPlus uses %1 throughout the interface.").arg(interfaceFont), font);
    // The text before the zoom; Appearance's Interface zoom scales it with the chrome.
    if (interfaceSize != nullptr)
        addTypographyRow(1, tr("Interface font size"), {}, interfaceSize);
    // Terminal text follows the editor and Markdown the interface, so neither has a row.
    if (editorFont != nullptr)
        addTypographyRow(3, tr("Editor font"), {}, editorFont);
    if (editorSize != nullptr)
        addTypographyRow(4, tr("Editor font size"), {}, editorSize);
    layout->addWidget(fields);
    chrome_geometry::addSpacing(layout, 24);
    if (auto* sample = dialog_->findChild<QWidget*>(QStringLiteral("typographyPreview"))) {
        sample->setParent(typography);
        sample->show();
        layout->addWidget(sample);
    }
    layout->addStretch(1);
    return typography;
}

void SettingsWorkspace::addTypographyRow(const int row, const QString& title, const QString& help,
                                         QWidget* control) {
    auto* fields = typographyFields_->parentWidget();
    control->setParent(fields);
    fitSelect(control);
    auto* copy = new QWidget(fields);
    auto* copyLayout = new QVBoxLayout(copy);
    chrome_geometry::margins(copyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(copyLayout, 2);
    copyLayout->addWidget(label(title, QStringLiteral("settingsWorkspaceFieldLabel"), copy));
    if (!help.isEmpty())
        copyLayout->addWidget(label(help, QStringLiteral("settingsWorkspaceHelp"), copy));
    const auto align = help.isEmpty() ? Qt::AlignVCenter : Qt::AlignTop;
    typographyFields_->addWidget(copy, row, 0, align);
    typographyFields_->addWidget(control, row, 1, Qt::AlignRight | align);
    control->show();
}

void SettingsWorkspace::addNavigation(const QString& title, const QString& id) {
    auto* item = new QListWidgetItem(title, navigation_);
    item->setData(Qt::UserRole, id);
    auto* control = new ket::NavItem(title, navigationIcon(id), navigation_);
    item->setSizeHint(control->sizeHint());
    navigation_->setItemWidget(item, control);
    const QPersistentModelIndex index(navigation_->model()->index(navigation_->row(item), 0));
    connect(control, &QAbstractButton::clicked, navigation_, [this, index] {
        if (index.isValid())
            navigation_->setCurrentRow(index.row());
    });
}

void SettingsWorkspace::addSection(const QString& title, const QString& id, QWidget* page,
                                   const bool restore) {
    auto* scroll = scrollPage(page, pages_);
    pages_->addWidget(scroll);
    sections_.insert(id, scroll);
    if (!restore)
        withoutRestore_.insert(id);
    addNavigation(title, id);
}

void SettingsWorkspace::addLink(const QString& title, const QString& id) {
    addNavigation(title, id);
}

// The dialog goes first. Its controls and pages now live on this page, and Qt deletes them
// before the dialog, its last child; a page's destruction would otherwise make the dialog read
// controls already deleted.
SettingsWorkspace::~SettingsWorkspace() { delete dialog_; }

void SettingsWorkspace::showSection(const QString& id) {
    for (int row = 0; row < navigation_->count(); ++row) {
        if (navigation_->item(row)->data(Qt::UserRole).toString() == id) {
            navigation_->setCurrentRow(row);
            return;
        }
    }
}

QString SettingsWorkspace::currentSection() const {
    const auto* item = navigation_->currentItem();
    return item == nullptr ? QString() : item->data(Qt::UserRole).toString();
}

void SettingsWorkspace::showRow(const int row) {
    for (int index = 0; index < navigation_->count(); ++index) {
        if (auto* control =
                qobject_cast<ket::NavItem*>(navigation_->itemWidget(navigation_->item(index))))
            control->setChecked(index == row);
    }
    const QString id = row < 0 ? QString() : navigation_->item(row)->data(Qt::UserRole).toString();
    if (auto* page = sections_.value(id)) {
        pages_->setCurrentWidget(page);
        if (footer_ != nullptr)
            footer_->setVisible(!withoutRestore_.contains(id));
    } else if (!id.isEmpty()) {
        emit linkActivated(id);
    }
}

void SettingsWorkspace::addStyleRules(const QString& rules) {
    styleRules_.append(rules);
    applyAppearance();
}

void SettingsWorkspace::applyAppearance() {
    // The zoom of the theme this page edits, as the window's other surfaces use it.
    const int zoom = dialog_->theme() != nullptr ? dialog_->theme()->interfaceZoomPercent()
                                                 : InterfaceZoomMetrics::current();
    const auto theme = ket::Appearance::instance().theme();
    const auto color = [theme](QStringView name) {
        return ket::Tokens::color(name, theme).name(QColor::HexArgb);
    };
    const QString sheet = TypeScale::apply(QStringLiteral(R"QSS(
QWidget#settingsWorkspacePage, QWidget#settingsWorkspaceContent,
QWidget#settingsWorkspaceHeading, QWidget#settingsWorkspaceBody,
QWidget#settingsWorkspaceAppearance, QWidget#settingsWorkspaceTypography,
QStackedWidget#settingsWorkspacePages, QScrollArea#settingsWorkspaceScroll,
QScrollArea#settingsWorkspaceScroll > QWidget > QWidget { background:%1; color:%2; }
QLabel#settingsWorkspaceEyebrow { color:%5; font-size:{{caption}}px; font-weight:600; letter-spacing:1px; }
QLabel#settingsWorkspaceTitle { color:%2; font-size:{{px:30}}px; font-weight:600; }
QLabel#settingsWorkspaceSubtitle, QLabel#settingsWorkspaceIntro { color:%3; font-size:{{reading}}px; }
QLabel#settingsWorkspaceHelp { color:%3; font-size:{{body}}px; }
QLabel#settingsWorkspaceSectionTitle { color:%2; font-size:{{px:22}}px; font-weight:500; }
QLabel#settingsWorkspaceSubheading { color:%2; font-size:{{reading}}px; font-weight:600; }
QLabel#settingsWorkspaceFieldLabel { color:%2; font-size:{{reading}}px; }
QListWidget#settingsWorkspaceNavigation { padding:0 20px 0 0; border:0;
 border-right:1px solid %4; background:transparent; outline:0; }
QListWidget#settingsWorkspaceNavigation::item { padding:0; border:0; background:transparent; color:transparent; }
QFrame#settingsWorkspaceDivider { border:0; background:%4; }
QFrame#settingsWorkspaceAccent { border:0; background:transparent; }
QLabel#settingsAccentCheck { color:%5; font-size:{{reading}}px; }
QLabel#settingsAccentTitle { color:%2; font-size:{{body}}px; font-weight:600; }
QWidget#settingsWorkspaceFooter { padding-top:18px; border-top:1px solid %4;
 background:transparent; }
QDialog[kvRole="settingsDialog"] { border:0; background:transparent; }
)QSS") + styleRules_,
                                           zoom);
    setStyleSheet(sheet.arg(color(u"page-bg"), color(u"text-main"), color(u"text-muted"),
                            color(u"panel-border"), color(u"accent-text")));
    chrome_geometry::apply(this, zoom);
}

} // namespace ketplus

#include "app/SettingsWorkspacePage.h"
#include "KetIcons.h"
#include "KetNavItem.h"
#include "KetSegmentedControl.h"
#include "KetSelect.h"
#include "KetThemeCard.h"
#include "ui/ChromeGeometry.h"
#include <QPersistentModelIndex>
#include <QSignalBlocker>

#include "agent/AgentWorkspaceView.h"
#include "app/AgentConversationWidth.h"
#include "app/SettingsBrowserPage.h"
#include "app/SettingsComputerPage.h"
#include "app/SettingsDialog.h"
#include "app/SettingsUpdatesPage.h"
#include "ui/InterfaceFont.h"
#include "ui/InterfaceZoom.h"
#include "ui/Theme.h"
#include "ui/TypeScale.h"
#include "update/AppUpdater.h"

#include "app/PixelSelect.h"
#include <QAbstractButton>
#include <QButtonGroup>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QScrollArea>
#include <QSizePolicy>
#include <QStackedWidget>
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

QByteArray navigationIconBody(const QString& id) {
    static const QHash<QString, QByteArray> names{
        {QStringLiteral("appearance"), "palette"},
        {QStringLiteral("typography"), "type"},
        {QStringLiteral("account-devices"), "monitor"},
        {QStringLiteral("providers"), "package"},
        {QStringLiteral("computer"), "monitor"},
        {QStringLiteral("browser"), "globe"},
        {QStringLiteral("storage"), "layers"},
        {QStringLiteral("updates"), "download"},
        {QStringLiteral("agent-models"), "sliders-horizontal"},
    };
    return names.value(id, "archive");
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
    auto* followSystem = dialog_->findChild<QAbstractButton*>(QStringLiteral("followSystemTheme"));

    auto* body = new QWidget(content);
    body->setObjectName(QStringLiteral("settingsWorkspaceBody"));
    auto* bodyLayout = new QHBoxLayout(body);
    chrome_geometry::margins(bodyLayout, 0, 0, 0, 0);
    chrome_geometry::spacing(bodyLayout, 35);
    auto* navigation = new QListWidget(body);
    navigation_ = navigation;
    navigation->setObjectName(QStringLiteral("settingsWorkspaceNavigation"));
    chrome_geometry::fixedWidth(navigation, 200);
    chrome_geometry::iconSize(navigation, QSize(14, 14));
    chrome_geometry::spacing(navigation, 2);
    navigation->setWordWrap(true);
    auto addNavigation = [navigation](const QString& title, const QString& id) {
        auto* item = new QListWidgetItem(title, navigation);
        item->setData(Qt::UserRole, id);
        item->setData(Qt::UserRole + 1, navigationIconBody(id));
        auto* control = new ket::NavItem(
            title, ket::Icons::codeForLucide(QString::fromLatin1(navigationIconBody(id))),
            navigation);
        item->setSizeHint(control->sizeHint());
        navigation->setItemWidget(item, control);
        const QPersistentModelIndex index(navigation->model()->index(navigation->row(item), 0));
        connect(control, &QAbstractButton::clicked, navigation, [navigation, index] {
            if (index.isValid())
                navigation->setCurrentRow(index.row());
        });
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
    auto* lightCard = dialog_->findChild<ket::ThemeCard*>(QStringLiteral("lightThemeCard"));
    auto* darkCard = dialog_->findChild<ket::ThemeCard*>(QStringLiteral("darkThemeCard"));
    for (auto* card : {lightCard, darkCard}) {
        card->setParent(themes);
        card->show();
    }
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
    auto* accentSwatch = new ket::AccentSwatch(accent);
    accentSwatch->setObjectName(QStringLiteral("settingsAccentSwatch"));
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
    auto* interfaceSize =
        dialog_->findChild<ketplus::PixelSelect*>(QStringLiteral("interfaceFontSize"));
    auto* editorSize = dialog_->findChild<ketplus::PixelSelect*>(QStringLiteral("editorFontSize"));
    auto* editorLine =
        dialog_->findChild<ketplus::PixelSelect*>(QStringLiteral("editorLineHeight"));
    auto* terminalSize =
        dialog_->findChild<ketplus::PixelSelect*>(QStringLiteral("terminalFontSize"));
    auto* terminalLine =
        dialog_->findChild<ketplus::PixelSelect*>(QStringLiteral("terminalLineHeight"));
    auto* previewSize =
        dialog_->findChild<ketplus::PixelSelect*>(QStringLiteral("previewFontSize"));
    auto* previewLine =
        dialog_->findChild<ketplus::PixelSelect*>(QStringLiteral("previewLineHeight"));
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
    auto* interfaceFont = new ket::Select(fields);
    interfaceFont->setObjectName(QStringLiteral("settingsInterfaceFont"));
    interfaceFont->addItem(InterfaceUiFont::settingsLabel());
    interfaceFont->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    auto* interfaceZoom =
        new ket::SegmentedControl({tr("Small"), tr("Medium"), tr("Large")}, fields);
    interfaceZoom->setObjectName(QStringLiteral("interfaceZoom"));
    interfaceZoom->setExpanding(true);
    const auto syncZoom = [interfaceSize, interfaceZoom] {
        const int pixels =
            interfaceSize ? interfaceSize->value() : InterfaceZoomMetrics::mediumPixels;
        const QList<int> values{InterfaceZoomMetrics::smallPixels,
                                InterfaceZoomMetrics::mediumPixels,
                                InterfaceZoomMetrics::largePixels};
        auto options = QStringList{tr("Small"), tr("Medium"), tr("Large")};
        int index = values.indexOf(pixels);
        if (index < 0) {
            options.append(tr("Custom (%1)").arg(pixels));
            index = 3;
        }
        const QSignalBlocker blocker(interfaceZoom);
        interfaceZoom->setOptions(options);
        interfaceZoom->setCurrentIndex(index);
    };
    connect(interfaceZoom, &ket::SegmentedControl::currentChanged, this,
            [interfaceSize](int index) {
                const QList<int> values{InterfaceZoomMetrics::smallPixels,
                                        InterfaceZoomMetrics::mediumPixels,
                                        InterfaceZoomMetrics::largePixels};
                if (interfaceSize && index >= 0 && index < values.size())
                    interfaceSize->setValue(values[index]);
            });
    if (interfaceSize)
        connect(interfaceSize, &ketplus::PixelSelect::valueChanged, this,
                [this, syncZoom](int pixels) {
                    syncZoom();
                    if (!palette_.pageBackground.isEmpty())
                        applyPalette(palette_, pixels);
                });
    syncZoom();
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
    QStringList widthLabels;
    for (const auto& choice : AgentConversationWidth::choices())
        widthLabels.append(choice.label);
    auto* conversationWidth = new ket::SegmentedControl(widthLabels, fields);
    conversationWidth->setObjectName(QStringLiteral("agentConversationWidth"));
    conversationWidth->setExpanding(true);
    connect(conversationWidth, &ket::SegmentedControl::currentChanged, widthState,
            [widthState](int index) {
                const auto choices = AgentConversationWidth::choices();
                if (index >= 0 && index < choices.size())
                    widthState->select(choices[index].pixels);
            });
    const auto syncWidth = [conversationWidth, widthState] {
        const QSignalBlocker blocker(conversationWidth);
        const auto choices = AgentConversationWidth::choices();
        for (qsizetype index = 0; index < choices.size(); ++index)
            if (choices[index].pixels == widthState->pixels())
                conversationWidth->setCurrentIndex(int(index));
    };
    connect(widthState, &SettingsPage::changed, conversationWidth, syncWidth);
    syncWidth();
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
    auto* sample = dialog_->findChild<QWidget*>(QStringLiteral("typographyPreview"));
    sample->setParent(typography);
    sample->show();
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

    // The public dialog's footer is a kit button row; Cancel has no meaning in a page.
    auto* settingsFooter = dialog_->findChild<QWidget*>(QStringLiteral("settingsButtons"));
    if (settingsFooter) {
        if (auto* cancel =
                settingsFooter->findChild<QAbstractButton*>(QStringLiteral("settingsCancelButton")))
            cancel->hide();
        settingsFooter->setParent(content);
        settingsFooter->setObjectName(QStringLiteral("settingsWorkspaceFooter"));
        contentLayout->addWidget(settingsFooter);
        settingsFooter->show();
    }

    connect(navigation, &QListWidget::currentRowChanged, this,
            [this, navigation, pages, updates, computer, storage, settingsFooter](const int row) {
                for (int index = 0; index < navigation->count(); ++index) {
                    if (auto* control = qobject_cast<ket::NavItem*>(
                            navigation->itemWidget(navigation->item(index))))
                        control->setChecked(index == row);
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
QLabel#settingsWorkspaceEyebrow { color:%7; font-size:{{caption}}px; font-weight:600; letter-spacing:1px; }
QLabel#settingsWorkspaceTitle { color:%2; font-size:{{px:30}}px; font-weight:600; }
QLabel#settingsWorkspaceSubtitle, QLabel#settingsWorkspaceIntro { color:%3; font-size:{{reading}}px; }
QLabel#settingsWorkspaceShortcut { color:%3; font-size:{{caption}}px; }
QLabel#settingsWorkspaceHelp { color:%3; font-size:{{body}}px; }
QLabel#settingsWorkspaceSectionTitle { color:%2; font-size:{{px:22}}px; font-weight:500; }
QLabel#settingsWorkspaceSubheading { color:%2; font-size:{{reading}}px; font-weight:600; }
QListWidget#settingsWorkspaceNavigation { padding:0 20px 0 0; border:0;
 border-right:1px solid %5; background:transparent; outline:0; }
QListWidget#settingsWorkspaceNavigation::item { padding:0; border:0; background:transparent; color:transparent; }
QFrame#settingsWorkspaceDivider { border:0; background:%5; }
QFrame#settingsWorkspaceAccent { border:0; background:transparent; }
QFrame#settingsWorkspacePreview {
 border:1px solid %5; border-radius:7px; background:%6; }
QLabel#settingsAccentCheck { color:%7; font-size:{{reading}}px; }
QLabel#settingsAccentTitle { color:%2; font-size:{{body}}px; font-weight:600; }
QLabel#settingsPreviewTabs { min-height:32px; padding:0 12px; border-bottom:2px solid %4;
 color:%2; background:%6; font-size:{{micro}}px; }
QLabel#settingsPreviewCode { padding:14px 17px; color:%3; font-family:monospace; font-size:{{body}}px; }
QLabel#settingsPreviewMessage { min-height:34px; padding:0 17px; border-top:1px solid %5;
 color:%2; background:%6; font-size:{{body}}px; }
QLabel#storageUsage,
QLabel#storageStatus, QComboBox#storageDetailDays, QLabel#settingsWorkspaceFieldLabel,
QComboBox#computerWorkDisplay { color:%2; font-size:{{reading}}px; }
QWidget#settingsWorkspaceFooter { padding-top:18px; border-top:1px solid %5;
 background:transparent; }
QDialog[kvRole="settingsDialog"] { border:0; background:transparent; }
)QSS"),
                                           interfaceFontSizePixels_);
    setStyleSheet(sheet.arg(palette.pageBackground, palette.textMain, palette.textMuted,
                            palette.accent, palette.border, palette.panelBackground,
                            palette.accentText));
    chrome_geometry::apply(this, interfaceFontSizePixels);
}

} // namespace ketplus

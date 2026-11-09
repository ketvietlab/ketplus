#include "atlas/AtlasTab.h"
#include "ui/ChromeGeometry.h"
#include "ui/LucideIcon.h"

#include "ui/TypeScale.h"

#include <QApplication>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QStyleOptionToolButton>
#include <QStyledItemDelegate>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWebView>
#include <QWebViewLoadingInfo>

#include <algorithm>

namespace ketplus {
namespace {
const QString ketAtlasPackage = QStringLiteral("ketatlas@0.6.1");

// Lucide ISC path data, kept private and limited to glyphs used by this surface.
constexpr auto networkIcon =
    R"(<rect x="16" y="16" width="6" height="6" rx="1"/><rect x="2" y="16" width="6" height="6" rx="1"/><rect x="9" y="2" width="6" height="6" rx="1"/><path d="M5 16v-3a1 1 0 0 1 1-1h12a1 1 0 0 1 1 1v3"/><path d="M12 12V8"/>)";
constexpr auto gridIcon =
    R"(<rect width="7" height="7" x="3" y="3" rx="1"/><rect width="7" height="7" x="14" y="3" rx="1"/><rect width="7" height="7" x="14" y="14" rx="1"/><rect width="7" height="7" x="3" y="14" rx="1"/>)";
constexpr auto listIcon = R"(<path d="M2 5h20"/><path d="M6 12h12"/><path d="M9 19h6"/>)";
constexpr auto folderIcon =
    R"(<path d="M20 20a2 2 0 0 0 2-2V8a2 2 0 0 0-2-2h-7.9a2 2 0 0 1-1.69-.9L9.6 3.9A2 2 0 0 0 7.93 3H4a2 2 0 0 0-2 2v13a2 2 0 0 0 2 2Z"/>)";

class LucideLabel final : public QLabel {
  public:
    explicit LucideLabel(QByteArray body, const int iconSize, QWidget* parent,
                         const QPalette::ColorRole role = QPalette::Highlight)
        : QLabel(parent), body_(std::move(body)), role_(role) {
        setFixedSize(iconSize, iconSize);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        lucide_icon::paint(painter, body_, palette().color(role_), rect());
    }

  private:
    QByteArray body_;
    QPalette::ColorRole role_;
};

class LucideButton final : public QToolButton {
  public:
    explicit LucideButton(QByteArray body, QWidget* parent)
        : QToolButton(parent), body_(std::move(body)) {}

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        QStyleOptionToolButton option;
        initStyleOption(&option);
        option.text.clear();
        option.icon = {};
        style()->drawComplexControl(QStyle::CC_ToolButton, &option, &painter, this);
        const QColor color = palette().color(QPalette::ButtonText);
        lucide_icon::paint(painter, body_, color,
                           QRectF((width() - 17) / 2.0, (height() - 17) / 2.0, 17, 17));
    }

  private:
    QByteArray body_;
};

QString atlasStyle(const QPalette& palette, const int basePixels) {
    const bool dark = palette.color(QPalette::Window).lightness() < 128;
    const QString page = dark ? QStringLiteral("#1B1F24") : QStringLiteral("#F7F5F5");
    const QString panel = dark ? QStringLiteral("#1D2228") : QStringLiteral("#FFFFFF");
    const QString raised = dark ? QStringLiteral("#23282F") : QStringLiteral("#FBFAFA");
    const QString text = dark ? QStringLiteral("#F2F4F7") : QStringLiteral("#24262A");
    const QString secondary = dark ? QStringLiteral("#B4BAC4") : QStringLiteral("#5A5C5E");
    const QString muted = dark ? QStringLiteral("#858D99") : QStringLiteral("#717373");
    const QString border = dark ? QStringLiteral("#2B3037") : QStringLiteral("#E9E7E8");
    const QString strong = dark ? QStringLiteral("#3B424C") : QStringLiteral("#CCCCCD");
    const QString accent = QStringLiteral("#1746FF");
    const QString accentText = dark ? QStringLiteral("#9EB5FF") : QStringLiteral("#1238DB");
    const QString primary = QStringLiteral("#365FD6");
    const QString primaryHover = QStringLiteral("#3151C6");
    QString css = TypeScale::apply(QStringLiteral(R"QSS(
QWidget#atlasTab, QWidget#atlasPicker, QStackedWidget#atlasPages { background:@page@; color:@text@; }
QLabel#atlasEyebrow { color:@accentText@; font-size:{{caption}}px; font-weight:600; letter-spacing:1px; }
QLabel#atlasLibraryTitle { color:@text@; font-size:{{display}}px; font-weight:620; }
QLabel#atlasLibrarySubtitle { color:@muted@; font-size:{{reading}}px; }
QLabel#atlasScopeIcon { color:@muted@; }
QLabel#atlasScope { color:@secondary@; font-size:{{body}}px; font-weight:600; }
QLabel#atlasCount, QLabel#atlasLibraryFootnote { color:@muted@; font-size:{{body}}px; }
QLineEdit#atlasSearch { min-height:34px; max-width:290px; padding:0 12px; border:1px solid @strong@; border-radius:6px; background:@panel@; color:@text@; }
QLineEdit#atlasSearch:focus { border-color:@accent@; }
QListWidget#atlasGrid { border:0; background:transparent; outline:0; }
QListWidget#atlasGrid::item { border:0; background:transparent; }
QPushButton#atlasCreateButton, QPushButton#atlasEmptyCreateButton { min-height:32px; padding:0 13px; border:1px solid @primary@; border-radius:6px; background:@primary@; color:white; font-weight:600; }
QPushButton#atlasCreateButton:hover, QPushButton#atlasEmptyCreateButton:hover { border-color:@primaryHover@; background:@primaryHover@; }
QToolButton[atlasViewToggle="true"] { min-width:30px; min-height:28px; border:0; border-radius:4px; background:transparent; color:@muted@; }
QToolButton[atlasViewToggle="true"]:checked { background:@panel@; color:@text@; }
QWidget#atlasViewSwitcher { padding:3px; border:1px solid @border@; border-radius:6px; background:@raised@; }
QWidget#atlasEmptyPanel { background:transparent; }
QLabel#atlasEmptyIcon { color:@accentText@; font-size:{{glyphLarge}}px; }
QLabel#atlasEmptyTitle { color:@text@; font-size:{{title}}px; font-weight:600; }
QLabel#atlasEmptyState { color:@muted@; font-size:{{reading}}px; }
QLabel#atlasStatus { color:@secondary@; font-size:{{reading}}px; }
QLabel#atlasLastValidNotice { color:@secondary@; background:@raised@; border-bottom:1px solid @border@; }
)QSS"),
                                   basePixels);
    const QList<QPair<QString, QString>> replacements = {
        {"@page@", page},           {"@panel@", panel},
        {"@raised@", raised},       {"@text@", text},
        {"@secondary@", secondary}, {"@muted@", muted},
        {"@border@", border},       {"@strong@", strong},
        {"@accent@", accent},       {"@accentText@", accentText},
        {"@primary@", primary},     {"@primaryHover@", primaryHover}};
    for (const auto& replacement : replacements)
        css.replace(replacement.first, replacement.second);
    return css;
}

QString npmPath() {
    return QStandardPaths::findExecutable(QStringLiteral("npm"), AtlasProcess::toolSearchPaths());
}

class AtlasCard final : public QStyledItemDelegate {
  public:
    using QStyledItemDelegate::QStyledItemDelegate;
    void paint(QPainter* p, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        p->save();
        const auto rect = option.rect.adjusted(8, 8, -8, -8);
        const bool selected = option.state.testFlag(QStyle::State_Selected);
        const bool dark = QApplication::palette().color(QPalette::Window).lightness() < 128;
        const QColor panel(dark ? QStringLiteral("#1D2228") : QStringLiteral("#FFFFFF"));
        const QColor selectedPanel(dark ? QStringLiteral("#202C4B") : QStringLiteral("#EDF2FF"));
        const QColor border(dark ? QStringLiteral("#2B3037") : QStringLiteral("#E9E7E8"));
        const QColor selectedBorder(dark ? QStringLiteral("#415B9C") : QStringLiteral("#C2D1FF"));
        const QColor text(dark ? QStringLiteral("#F2F4F7") : QStringLiteral("#24262A"));
        const QColor muted(dark ? QStringLiteral("#858D99") : QStringLiteral("#717373"));
        p->setPen(selected ? selectedBorder : border);
        p->setBrush(selected ? selectedPanel : panel);
        p->drawRoundedRect(rect, 8, 8);
        const QRect previewRect(rect.x() + 1, rect.y() + 1, rect.width() - 2, 179);
        const QPixmap thumbnail = index.data(Qt::DecorationRole).value<QPixmap>();
        if (!thumbnail.isNull()) {
            p->setClipRect(previewRect);
            p->drawPixmap(previewRect, thumbnail, thumbnail.rect());
            p->setClipping(false);
        } else {
            p->fillRect(previewRect, option.palette.color(QPalette::AlternateBase));
        }
        p->setPen(text);
        auto font = option.font;
        font.setBold(true);
        p->setFont(font);
        p->drawText(rect.adjusted(17, 196, -17, -70), Qt::TextWordWrap,
                    index.data(Qt::DisplayRole).toString());
        font.setBold(false);
        p->setFont(font);
        p->setPen(muted);
        const int width = rect.width() - 24;
        p->drawText(rect.adjusted(17, 225, -17, -45), Qt::AlignVCenter,
                    QFontMetrics(font).elidedText(index.data(Qt::UserRole + 3).toString(),
                                                  Qt::ElideRight, width));
        p->drawText(rect.adjusted(17, 257, -17, -15), Qt::AlignVCenter,
                    QFontMetrics(font).elidedText(index.data(Qt::UserRole + 2).toString(),
                                                  Qt::ElideRight, width));
        p->restore();
    }
};
} // namespace

AtlasTab::AtlasTab(AtlasIndex* index, QWidget* parent)
    : QWidget(parent), index_(index), process_(this), search_(new QLineEdit(this)),
      grid_(new QListWidget(this)), count_(new QLabel(this)), scope_(new QLabel(this)),
      empty_(new QLabel(this)), status_(new QLabel(this)), installCommand_(new QLabel(this)),
      lastValidNotice_(new QLabel(this)), retry_(new QPushButton(tr("Retry"), this)),
      locate_(new QPushButton(tr("Locate KetAtlas CLI"), this)),
      install_(new QPushButton(tr("Install KetAtlas"), this)),
      chooseAnother_(new QPushButton(tr("Choose another Atlas"), this)),
      pages_(new QStackedWidget(this)), picker_(new QWidget(this)), messagePage_(new QWidget(this)),
      thumbnails_(this) {
    setObjectName("atlasTab");
    setAttribute(Qt::WA_StyledBackground, true);
    applyPalette(QApplication::palette());
    auto* layout = new QVBoxLayout(this);
    chrome_geometry::margins(layout, 0, 0, 0, 0);
    chrome_geometry::spacing(layout, 0);
    lastValidNotice_->setObjectName(QStringLiteral("atlasLastValidNotice"));
    lastValidNotice_->setText(
        tr("Showing the last valid preview while bundle changes are validated."));
    lastValidNotice_->setAlignment(Qt::AlignCenter);
    chrome_geometry::margins(lastValidNotice_, 12, 7, 12, 7);
    lastValidNotice_->hide();
    layout->addWidget(lastValidNotice_);
    layout->addWidget(pages_, 1);
    auto* pickerLayout = new QVBoxLayout(picker_);
    picker_->setObjectName(QStringLiteral("atlasPicker"));
    pages_->setObjectName(QStringLiteral("atlasPages"));
    chrome_geometry::margins(pickerLayout, 42, 36, 42, 38);
    chrome_geometry::spacing(pickerLayout, 0);
    auto* pageHeader = new QHBoxLayout;
    auto* headingCopy = new QVBoxLayout;
    chrome_geometry::spacing(headingCopy, 7);
    auto* eyebrow = new QLabel(tr("FROM IDEA TO EXPERIENCE"), picker_);
    eyebrow->setObjectName(QStringLiteral("atlasEyebrow"));
    headingCopy->addWidget(eyebrow);
    auto* heading = new QLabel(tr("Your Atlases"), picker_);
    heading->setObjectName(QStringLiteral("atlasLibraryTitle"));
    headingCopy->addWidget(heading);
    auto* subtitle = new QLabel(tr("Design, test, and connect screens in a single flow."), picker_);
    subtitle->setObjectName(QStringLiteral("atlasLibrarySubtitle"));
    headingCopy->addWidget(subtitle);
    // The folder listed, so the library never reads as another repository's Atlases.
    auto* scopeRow = new QHBoxLayout;
    chrome_geometry::margins(scopeRow, 0, 2, 0, 0);
    chrome_geometry::spacing(scopeRow, 6);
    auto* scopeIcon = new LucideLabel(folderIcon, 14, picker_, QPalette::WindowText);
    scopeIcon->setObjectName(QStringLiteral("atlasScopeIcon"));
    scopeRow->addWidget(scopeIcon);
    scope_->setObjectName(QStringLiteral("atlasScope"));
    scopeRow->addWidget(scope_);
    scopeRow->addStretch(1);
    headingCopy->addLayout(scopeRow);
    pageHeader->addLayout(headingCopy, 1);
    auto* create = new QPushButton(tr("Create Atlas"), picker_);
    create->setObjectName(QStringLiteral("atlasCreateButton"));
    create->setIcon(QIcon(QStringLiteral(":/atlas/icons/plus.svg")));
    pageHeader->addWidget(create);
    pickerLayout->addLayout(pageHeader);
    chrome_geometry::addSpacing(pickerLayout, 27);
    search_->setObjectName("atlasSearch");
    search_->setPlaceholderText(tr("Search Atlases in this workspace…"));
    search_->setClearButtonEnabled(true);
    auto* pickerHeader = new QHBoxLayout;
    chrome_geometry::margins(pickerHeader, 0, 0, 0, 0);
    chrome_geometry::spacing(pickerHeader, 8);
    pickerHeader->addWidget(search_);
    pickerHeader->addStretch(1);
    pickerHeader->addWidget(count_);
    auto* switcher = new QWidget(picker_);
    switcher->setObjectName(QStringLiteral("atlasViewSwitcher"));
    auto* switcherLayout = new QHBoxLayout(switcher);
    chrome_geometry::margins(switcherLayout, 3, 3, 3, 3);
    chrome_geometry::spacing(switcherLayout, 3);
    auto* gridMode = new LucideButton(gridIcon, switcher);
    gridMode->setObjectName(QStringLiteral("atlasGridModeButton"));
    gridMode->setToolTip(tr("Grid view"));
    gridMode->setAccessibleName(tr("Grid view"));
    gridMode->setCheckable(true);
    gridMode->setChecked(true);
    gridMode->setProperty("atlasViewToggle", true);
    auto* listMode = new LucideButton(listIcon, switcher);
    listMode->setObjectName(QStringLiteral("atlasListModeButton"));
    listMode->setToolTip(tr("List view"));
    listMode->setAccessibleName(tr("List view"));
    listMode->setCheckable(true);
    listMode->setProperty("atlasViewToggle", true);
    switcherLayout->addWidget(gridMode);
    switcherLayout->addWidget(listMode);
    pickerHeader->addWidget(switcher);
    pickerLayout->addLayout(pickerHeader);
    chrome_geometry::addSpacing(pickerLayout, 9);
    count_->setObjectName("atlasCount");
    grid_->setObjectName("atlasGrid");
    grid_->setViewMode(QListView::IconMode);
    grid_->setResizeMode(QListView::Adjust);
    grid_->setMovement(QListView::Static);
    grid_->setGridSize(QSize(260, 300));
    grid_->setSelectionMode(QAbstractItemView::SingleSelection);
    grid_->setItemDelegate(new AtlasCard(grid_));
    grid_->setFrameShape(QFrame::NoFrame);
    pickerLayout->addWidget(grid_, 1);
    auto* emptyPanel = new QWidget(picker_);
    emptyPanel->setObjectName(QStringLiteral("atlasEmptyPanel"));
    auto* emptyLayout = new QVBoxLayout(emptyPanel);
    emptyLayout->setAlignment(Qt::AlignCenter);
    chrome_geometry::spacing(emptyLayout, 14);
    auto* emptyIcon = new LucideLabel(networkIcon, 35, emptyPanel);
    emptyIcon->setObjectName(QStringLiteral("atlasEmptyIcon"));
    emptyIcon->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyIcon);
    auto* emptyTitle = new QLabel(tr("Your first idea is waiting."), emptyPanel);
    emptyTitle->setObjectName(QStringLiteral("atlasEmptyTitle"));
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle);
    empty_->setObjectName("atlasEmptyState");
    empty_->setText(tr("Describe what you want to build. Your Agent will create screens and "
                       "interactive flows in this workspace."));
    empty_->setAlignment(Qt::AlignCenter);
    empty_->setWordWrap(true);
    chrome_geometry::maximumWidth(empty_, 390);
    emptyLayout->addWidget(empty_);
    auto* emptyCreate = new QPushButton(tr("Create your first Atlas"), emptyPanel);
    emptyCreate->setObjectName(QStringLiteral("atlasEmptyCreateButton"));
    connect(emptyCreate, &QPushButton::clicked, this, &AtlasTab::creationRequested);
    emptyLayout->addWidget(emptyCreate, 0, Qt::AlignHCenter);
    pickerLayout->addWidget(emptyPanel, 1);
    auto* footnote = new QLabel(
        tr("Bundles in this worktree · Updates automatically when files change"), picker_);
    footnote->setObjectName(QStringLiteral("atlasLibraryFootnote"));
    pickerLayout->addWidget(footnote);
    auto* messageLayout = new QVBoxLayout(messagePage_);
    status_->setObjectName("atlasStatus");
    status_->setWordWrap(true);
    status_->setAlignment(Qt::AlignCenter);
    messageLayout->addStretch();
    messageLayout->addWidget(status_);
    installCommand_->setObjectName(QStringLiteral("atlasInstallCommand"));
    installCommand_->setText(QStringLiteral("npm install -g %1").arg(ketAtlasPackage));
    installCommand_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    installCommand_->setAlignment(Qt::AlignCenter);
    installCommand_->hide();
    messageLayout->addWidget(installCommand_);
    retry_->setObjectName(QStringLiteral("atlasRetryButton"));
    locate_->setObjectName(QStringLiteral("atlasLocateCliButton"));
    install_->setObjectName(QStringLiteral("atlasInstallButton"));
    install_->hide();
    messageLayout->addWidget(install_, 0, Qt::AlignCenter);
    messageLayout->addWidget(retry_, 0, Qt::AlignCenter);
    messageLayout->addWidget(locate_, 0, Qt::AlignCenter);
    chooseAnother_->setObjectName(QStringLiteral("atlasChooseAnotherButton"));
    chooseAnother_->hide();
    messageLayout->addWidget(chooseAnother_, 0, Qt::AlignCenter);
    messageLayout->addStretch();
    pages_->addWidget(picker_);
    pages_->addWidget(messagePage_);
    connect(retry_, &QPushButton::clicked, this, &AtlasTab::launch);
    connect(install_, &QPushButton::clicked, this, &AtlasTab::installKetAtlas);
    connect(chooseAnother_, &QPushButton::clicked, this, &AtlasTab::returnToLibrary);
    connect(create, &QPushButton::clicked, this, &AtlasTab::creationRequested);
    connect(locate_, &QPushButton::clicked, this, [this] {
        const auto file = QFileDialog::getOpenFileName(this, tr("KetAtlas executable"));
        if (!file.isEmpty()) {
            QSettings().setValue("atlas/cliPath", file);
            launch();
        }
    });
    connect(index_, &AtlasIndex::changed, this, &AtlasTab::handleIndexChanged);
    connect(search_, &QLineEdit::textChanged, this, &AtlasTab::populate);
    connect(gridMode, &QToolButton::clicked, this, [this, gridMode, listMode] {
        gridMode->setChecked(true);
        listMode->setChecked(false);
        grid_->setViewMode(QListView::IconMode);
        grid_->setProperty("atlasListMode", false);
        resizeEvent(nullptr);
    });
    connect(listMode, &QToolButton::clicked, this, [this, gridMode, listMode] {
        listMode->setChecked(true);
        gridMode->setChecked(false);
        grid_->setViewMode(QListView::IconMode);
        grid_->setProperty("atlasListMode", true);
        resizeEvent(nullptr);
    });
    connect(grid_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            &AtlasTab::ensureVisibleThumbnails);
    connect(&thumbnails_, &AtlasThumbnailCache::thumbnailReady, this,
            [this](const QString& key, const QImage& image) {
                for (int row = 0; row < grid_->count(); ++row) {
                    auto* item = grid_->item(row);
                    if (item->data(Qt::UserRole + 4).toString() == key)
                        item->setData(Qt::DecorationRole, QPixmap::fromImage(image));
                }
            });
    const auto choose = [this](QListWidgetItem* item) {
        const auto path = item->data(Qt::UserRole).toString();
        for (const auto& file : index_->atlases())
            if (file.path == path) {
                emit atlasRequested(file);
                break;
            }
    };
    connect(grid_, &QListWidget::itemClicked, this, choose);
    connect(grid_, &QListWidget::itemActivated, this, choose);
    connect(&process_, &AtlasProcess::failed, this, &AtlasTab::showError);
    connect(&process_, &AtlasProcess::ready, this, [this](const QUrl& url) {
        auto target = url;
        target.setQuery(savedQuery_);
        browser_ = new QWebView;
        browserContainer_ = QWidget::createWindowContainer(browser_, pages_);
        pages_->addWidget(browserContainer_);
        pages_->setCurrentWidget(browserContainer_);
        connect(browser_, &QWebView::loadingChanged, this, [this](const QWebViewLoadingInfo& info) {
            if (info.status() == QWebViewLoadingInfo::LoadStatus::Failed)
                showError(info.errorString());
            if (info.status() == QWebViewLoadingInfo::LoadStatus::Succeeded)
                restoreTimer_.start();
        });
        connect(browser_, &QWebView::urlChanged, this,
                [this](const QUrl& value) { savedQuery_ = value.query(); });
        browser_->setUrl(target);
        flowProbe_.start();
        probeFlowContext();
    });
    idle_.setSingleShot(true);
    idle_.setInterval(5 * 60 * 1000);
    connect(&idle_, &QTimer::timeout, this, &AtlasTab::defer);
    restoreTimer_.setInterval(250);
    connect(&restoreTimer_, &QTimer::timeout, this, [this] {
        if (!browser_)
            return;
        QPointer<AtlasTab> self(this);
        browser_->runJavaScript("Boolean(window.atlas)", [self](const QVariant& ready) {
            if (!self || !ready.toBool() || !self->browser_)
                return;
            self->restoreTimer_.stop();
            const auto json =
                QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(self->savedState_))
                                      .toJson(QJsonDocument::Compact));
            self->browser_->runJavaScript(
                QStringLiteral("(()=>{const "
                               "s=%1;try{if(s.flowId)atlas.goToFlow(s.flowId);if(s.selectedNodeId)"
                               "atlas.focusNode(s.flowId,s.selectedNodeId);if(s.zoom)atlas.zoomTo("
                               "s.zoom);}catch(e){}})()")
                    .arg(json));
            emit self->loaded();
        });
    });
    flowProbe_.setInterval(700);
    connect(&flowProbe_, &QTimer::timeout, this, &AtlasTab::probeFlowContext);
    missingBundle_.setSingleShot(true);
    missingBundle_.setInterval(1500);
    connect(&missingBundle_, &QTimer::timeout, this, [this] {
        if (atlas_.path.isEmpty())
            return;
        if (QFileInfo::exists(atlas_.path)) {
            // The manifest may be an invalid in-progress write. Continue keeping
            // the last valid preview, while still detecting a later deletion.
            missingBundle_.start();
            return;
        }
        releaseBrowser();
        process_.stop();
        lastValidNotice_->hide();
        install_->hide();
        installCommand_->hide();
        retry_->hide();
        locate_->hide();
        chooseAnother_->show();
        status_->setText(tr("This Atlas bundle is no longer available in the current worktree. "
                            "Choose another Atlas; the Builder conversation is preserved."));
        pages_->setCurrentWidget(messagePage_);
    });
    populate();
}

AtlasTab::~AtlasTab() {
    releaseBrowser();
    process_.stop();
}

void AtlasTab::populate() {
    const QString root = index_->workspaceRoot();
    scope_->setText(root.isEmpty() ? tr("No folder open") : QFileInfo(root).fileName());
    scope_->setToolTip(root);
    if (!atlas_.path.isEmpty())
        return;
    const auto atlases = index_->atlases();
    const bool hasAtlases = !atlases.isEmpty();
    search_->setVisible(hasAtlases);
    count_->setVisible(hasAtlases);
    grid_->setVisible(hasAtlases);
    empty_->setVisible(!hasAtlases);
    empty_->parentWidget()->setVisible(!hasAtlases);
    if (auto* footnote = picker_->findChild<QLabel*>(QStringLiteral("atlasLibraryFootnote")))
        footnote->setVisible(hasAtlases);
    if (!hasAtlases) {
        grid_->clear();
        count_->clear();
        return;
    }
    const auto selected =
        grid_->currentItem() ? grid_->currentItem()->data(Qt::UserRole).toString() : QString{};
    grid_->clear();
    const auto terms = search_->text().simplified().split(' ', Qt::SkipEmptyParts);
    for (const auto& file : atlases) {
        const auto text = file.title + ' ' + file.relativePath + ' ' + file.path;
        bool match = true;
        for (const auto& term : terms)
            if (!text.contains(term, Qt::CaseInsensitive))
                match = false;
        if (!match)
            continue;
        auto* item = new QListWidgetItem(file.title, grid_);
        item->setData(Qt::UserRole, file.path);
        item->setData(Qt::UserRole + 1, file.relativePath);
        item->setData(Qt::UserRole + 2,
                      tr("%1 screens   %2 flows").arg(file.screens.size()).arg(file.flowCount));
        item->setData(Qt::UserRole + 3,
                      file.description.isEmpty() ? file.relativePath : file.description);
        item->setData(Qt::UserRole + 4, AtlasThumbnailCache::cacheKey(file));
        item->setToolTip(file.title + "\n" + file.path);
        item->setSizeHint(grid_->gridSize());
        if (file.path == selected)
            grid_->setCurrentItem(item);
    }
    count_->setText(tr("%1 Atlases").arg(grid_->count()));
    QTimer::singleShot(0, this, &AtlasTab::ensureVisibleThumbnails);
}

void AtlasTab::handleIndexChanged() {
    if (atlas_.path.isEmpty()) {
        populate();
        return;
    }
    for (const auto& descriptor : index_->atlases()) {
        if (descriptor.path != atlas_.path)
            continue;
        missingBundle_.stop();
        lastValidNotice_->hide();
        atlas_ = descriptor;
        emit titleChanged(atlas_.title);
        QString title = preferredFlowId_;
        for (const auto& flow : atlas_.flows)
            if (flow.id == preferredFlowId_)
                title = flow.title;
        emit flowContextChanged(atlasId(), preferredFlowId_, title);
        return;
    }
    // Atomic saves and partially written manifests temporarily disappear from
    // the valid index. Keep the last valid preview in both cases; only a path
    // that remains physically absent transitions to the recovery state.
    if (!missingBundle_.isActive())
        missingBundle_.start();
    if (QFileInfo::exists(atlas_.path))
        lastValidNotice_->show();
}

void AtlasTab::ensureVisibleThumbnails() {
    if (!atlas_.path.isEmpty() || !grid_->isVisible())
        return;
    const QRect viewport = grid_->viewport()->rect().adjusted(0, -238, 0, 238);
    const auto descriptors = index_->atlases();
    for (int row = 0; row < grid_->count(); ++row) {
        auto* item = grid_->item(row);
        if (!grid_->visualItemRect(item).intersects(viewport) && row >= 12)
            continue;
        const QString path = item->data(Qt::UserRole).toString();
        const auto match = std::find_if(descriptors.cbegin(), descriptors.cend(),
                                        [&path](const auto& value) { return value.path == path; });
        if (match == descriptors.cend())
            continue;
        const QImage image = thumbnails_.request(*match);
        if (!image.isNull())
            item->setData(Qt::DecorationRole, QPixmap::fromImage(image));
    }
}

void AtlasTab::probeFlowContext() {
    if (!browser_)
        return;
    QPointer<AtlasTab> self(this);
    QPointer<QWebView> browser(browser_);
    browser_->runJavaScript("window.atlas?.getState()?.flowId || ''",
                            [self, browser](const QVariant& result) {
                                if (!self || self->browser_ != browser)
                                    return;
                                const QString flowId = result.toString();
                                if (flowId.isEmpty() || flowId == self->preferredFlowId_)
                                    return;
                                self->preferredFlowId_ = flowId;
                                QString title = flowId;
                                for (const auto& flow : self->atlas_.flows)
                                    if (flow.id == flowId)
                                        title = flow.title;
                                emit self->flowContextChanged(self->atlasId(), flowId, title);
                            });
}

void AtlasTab::returnToLibrary() {
    releaseBrowser();
    process_.stop();
    missingBundle_.stop();
    lastValidNotice_->hide();
    atlas_ = {};
    preferredFlowId_.clear();
    emit flowContextChanged({}, {}, {});
    chooseAnother_->hide();
    pages_->setCurrentWidget(picker_);
    emit titleChanged(tr("Atlases"));
    populate();
}

void AtlasTab::resizeEvent(QResizeEvent* event) {
    if (event)
        QWidget::resizeEvent(event);
    const int available = qMax(180, grid_->viewport()->width() - 8);
    // The KétAtlas library uses a compact three-card rhythm at the ordinary
    // desktop workspace width. Fall back progressively without forcing a
    // horizontal scrollbar in smaller windows.
    const int columns = available >= 570 ? 3 : available >= 380 ? 2 : 1;
    const bool listMode = grid_->property("atlasListMode").toBool();
    const auto size = listMode ? QSize(available, 300) : QSize(qMax(180, available / columns), 300);
    grid_->setGridSize(size);
    for (int row = 0; row < grid_->count(); ++row)
        grid_->item(row)->setSizeHint(size);
    ensureVisibleThumbnails();
}

void AtlasTab::applyPalette(const QPalette& palette, const int interfaceFontSizePixels) {
    if (applyingStyle_)
        return;
    applyingStyle_ = true;
    setStyleSheet(atlasStyle(palette, interfaceFontSizePixels));
    chrome_geometry::apply(this, interfaceFontSizePixels);
    grid_->viewport()->update();
    applyingStyle_ = false;
}

void AtlasTab::changeEvent(QEvent* event) {
    if (event->type() == QEvent::PaletteChange ||
        event->type() == QEvent::ApplicationPaletteChange ||
        event->type() == QEvent::ApplicationFontChange || event->type() == QEvent::FontChange) {
        const int basePixels = QApplication::font().pixelSize() > 0
                                   ? QApplication::font().pixelSize()
                                   : InterfaceZoomMetrics::mediumPixels;
        applyPalette(QApplication::palette(), basePixels);
    }
    QWidget::changeEvent(event);
}

void AtlasTab::openAtlas(const AtlasDescriptor& atlas) {
    if (atlas_.path != atlas.path) {
        savedState_.clear();
        savedQuery_.clear();
    }
    atlas_ = atlas;
    missingBundle_.stop();
    lastValidNotice_->hide();
    chooseAnother_->hide();
    preferredFlowId_ = atlas_.flows.isEmpty() ? QString{} : atlas_.flows.first().id;
    emit flowContextChanged(atlasId(), preferredFlowId_,
                            atlas_.flows.isEmpty() ? QString{} : atlas_.flows.first().title);
    emit titleChanged(atlas.title);
    launch();
}

void AtlasTab::refreshPreview() {
    if (!browser_) {
        // A first manifest may be syntactically valid before KetAtlas can load it.
        // Retry after subsequent edits, but never restart an in-flight launch.
        if (active_ && !deferred_ && !process_.running() &&
            QFileInfo(AtlasProcess::cliPath()).isExecutable())
            launch();
        return;
    }
    QPointer<AtlasTab> self(this);
    QPointer<QWebView> browser(browser_);
    browser_->runJavaScript(
        "JSON.stringify({state:window.atlas?.getState(),editing:!!window.atlas?.element?"
        ".shadowRoot?.querySelector('#progress-editor[open]')})",
        [self, browser](const QVariant& value) {
            if (!self || !browser || self->browser_ != browser)
                return;
            const auto snapshot = QJsonDocument::fromJson(value.toString().toUtf8()).object();
            if (snapshot.value("editing").toBool())
                return; // Do not discard an unsaved progress-editor draft.
            self->savedState_ = snapshot.value("state").toObject().toVariantMap();
            browser->reload(); // Keep the server, tab and Agent Workspace intact.
        });
}

void AtlasTab::launch() {
    if (atlas_.path.isEmpty())
        return;
    releaseBrowser();
    deferred_ = false;
    const QString cli = AtlasProcess::cliPath();
    if (cli.isEmpty() || !QFileInfo(cli).isExecutable()) {
        showMissingCli();
        return;
    }
    install_->hide();
    installCommand_->hide();
    chooseAnother_->hide();
    retry_->show();
    status_->setText(tr("Starting %1...").arg(atlas_.title));
    pages_->setCurrentWidget(messagePage_);
    emit titleChanged(atlas_.title);
    process_.start(atlas_);
    if (!active_)
        idle_.start();
}

void AtlasTab::installKetAtlas() {
    if (installer_ && installer_->state() != QProcess::NotRunning)
        return;
    const QString npm = npmPath();
    if (npm.isEmpty()) {
        showMissingCli();
        status_->setText(tr("npm was not found. Install Node.js 22+ and npm, then retry or locate "
                            "an existing KetAtlas executable."));
        return;
    }

    if (!installer_) {
        installer_ = new QProcess(this);
        installer_->setObjectName(QStringLiteral("ketAtlasInstaller"));
        installer_->setProcessChannelMode(QProcess::MergedChannels);
        connect(installer_, &QProcess::readyReadStandardOutput, this, [this] {
            installOutput_ = (installOutput_ + installer_->readAllStandardOutput()).right(8000);
        });
        connect(installer_, &QProcess::finished, this,
                [this](const int code, const QProcess::ExitStatus exitStatus) {
                    install_->setEnabled(true);
                    install_->setText(tr("Install KetAtlas"));
                    locate_->setEnabled(true);
                    retry_->setEnabled(true);
                    if (code == 0 && exitStatus == QProcess::NormalExit) {
                        launch();
                        return;
                    }
                    showMissingCli();
                    status_->setText(
                        tr("KetAtlas installation failed (exit %1).\n%2")
                            .arg(code)
                            .arg(QString::fromLocal8Bit(installOutput_).trimmed().right(2000)));
                });
        connect(installer_, &QProcess::errorOccurred, this,
                [this](const QProcess::ProcessError error) {
                    if (error != QProcess::FailedToStart)
                        return;
                    showMissingCli();
                    status_->setText(tr("Unable to start npm: %1").arg(installer_->errorString()));
                });
    }

    auto environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("PATH"), QFileInfo(npm).absolutePath() +
                                                   QDir::listSeparator() +
                                                   environment.value(QStringLiteral("PATH")));
    installer_->setProcessEnvironment(environment);
    installer_->setWorkingDirectory(QDir::homePath());
    installOutput_.clear();
    install_->setEnabled(false);
    install_->setText(tr("Installing KetAtlas..."));
    locate_->setEnabled(false);
    retry_->setEnabled(false);
    status_->setText(tr("Installing KetAtlas with npm..."));
    installer_->start(npm, {QStringLiteral("install"), QStringLiteral("-g"), ketAtlasPackage});
}

void AtlasTab::setActive(bool active) {
    active_ = active;
    if (active) {
        idle_.stop();
        if (deferred_)
            launch();
    } else if (!atlas_.path.isEmpty() && !deferred_ && !idle_.isActive())
        idle_.start();
}

void AtlasTab::defer() {
    if (active_ || atlas_.path.isEmpty() || deferred_)
        return;
    const auto finish = [this] {
        releaseBrowser();
        process_.stop();
        deferred_ = true;
        status_->setText(tr("Atlas suspended"));
        pages_->setCurrentWidget(messagePage_);
        emit titleChanged(atlas_.title + tr(" (Suspended)"));
    };
    if (!browser_) {
        finish();
        return;
    }
    QPointer<AtlasTab> self(this);
    // Keep an open progress editor resident rather than discarding an unsaved draft.
    browser_->runJavaScript("JSON.stringify({state:window.atlas?.getState(),editing:!!window.atlas?"
                            ".element?.shadowRoot?.querySelector('#progress-editor[open]')})",
                            [self, finish](const QVariant& value) {
                                if (!self || self->active_)
                                    return;
                                const auto state =
                                    QJsonDocument::fromJson(value.toString().toUtf8()).object();
                                if (state.value("editing").toBool()) {
                                    self->idle_.start();
                                    return;
                                }
                                self->savedState_ = state.value("state").toObject().toVariantMap();
                                finish();
                            });
}

void AtlasTab::releaseBrowser() {
    restoreTimer_.stop();
    flowProbe_.stop();
    if (browserContainer_) {
        pages_->removeWidget(browserContainer_);
        delete browserContainer_;
        browserContainer_ = nullptr;
        browser_ = nullptr;
    }
}

void AtlasTab::showError(const QString& error) {
    install_->hide();
    installCommand_->hide();
    retry_->show();
    chooseAnother_->hide();
    status_->setText(error);
    pages_->setCurrentWidget(messagePage_);
}

void AtlasTab::showMissingCli() {
    process_.stop();
    status_->setText(
        tr("KetAtlas is required to preview this Atlas. Install it with npm, or locate an "
           "existing KetAtlas executable. Atlas source files remain available in the worktree."));
    installCommand_->show();
    install_->show();
    retry_->show();
    locate_->show();
    chooseAnother_->hide();
    pages_->setCurrentWidget(messagePage_);
}
} // namespace ketplus

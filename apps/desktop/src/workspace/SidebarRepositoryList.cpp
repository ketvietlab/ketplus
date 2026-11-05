#include "workspace/SidebarRepositoryList.h"
#include "ui/ChromeGeometry.h"
#include "ui/InterfaceZoom.h"

#include "workspace/LucideIcons.h"

#include <QEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QSet>
#include <QSettings>
#include <QSizePolicy>
#include <QStyle>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

namespace ketplus {
namespace {

constexpr int kHeaderHeight = 28;
constexpr int kConversationHeight = 36;
constexpr int kConversationSpacing = 2;
constexpr int kGroupSpacing = 12;
constexpr int kRepositoryNameWidth = 150;
constexpr int kPreviewConversationCount = 5;
constexpr int kShowMoreHeight = 26;
const auto kCollapsedSetting = QStringLiteral("sidebar/collapsedRepositories");

// Elides to its current width so titles reach the sidebar edge at any sidebar width.
class ElidedLabel final : public QLabel {
  public:
    ElidedLabel(QString text, QWidget* parent) : QLabel(parent), fullText_(std::move(text)) {
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        setMinimumWidth(1);
    }

  protected:
    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            setText(fontMetrics().elidedText(fullText_, Qt::ElideRight, width()));
    }
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        setText(fontMetrics().elidedText(fullText_, Qt::ElideRight, width()));
    }

  private:
    QString fullText_;
};

struct ConversationPresentation final {
    QString label;
    QString tone;
    QByteArray icon;
    bool spinning{false};
};

ConversationPresentation conversationPresentation(const QVariantMap& conversation,
                                                  const bool seen) {
    const QString status = conversation.value(QStringLiteral("status")).toString().toLower();
    if (conversation.value(QStringLiteral("needsInput")).toBool() ||
        status.contains(QStringLiteral("input")) || status.contains(QStringLiteral("question")) ||
        status == QStringLiteral("waiting for your answer…"))
        return {QStringLiteral("Needs input"), QStringLiteral("warning"), lucide::pauseCircle};
    if (conversation.value(QStringLiteral("running")).toBool())
        // The main text colour: the accent loader was too dark to read on the dark sidebar.
        return {QStringLiteral("Working"), QStringLiteral("main"), lucide::loaderCircle, true};
    if (status.contains(QStringLiteral("fail")) || status.contains(QStringLiteral("error")) ||
        status.contains(QStringLiteral("disconnect")) ||
        status.contains(QStringLiteral("missing")) || status.contains(QStringLiteral("not found")))
        return {QStringLiteral("Disconnected"), QStringLiteral("danger"), lucide::triangleAlert};
    if (status.contains(QStringLiteral("stop")))
        return {QStringLiteral("Stopped"), QStringLiteral("muted"), lucide::square};
    if (!conversation.value(QStringLiteral("started")).toBool())
        return {QStringLiteral("Not started"), QStringLiteral("muted"), lucide::circle};
    if (!conversation.value(QStringLiteral("selected")).toBool() && !seen)
        return {QStringLiteral("Completed, unread"), QStringLiteral("accent"), lucide::circleCheck};
    return {QStringLiteral("Completed"), QStringLiteral("muted"), lucide::circleCheck};
}

// QLabel has no rotation property. Paint the normal Lucide pixmap through a rotating painter so
// the loader remains crisp at its native device-pixel ratio and stops immediately with the turn.
class RotatingIconLabel final : public QLabel {
  public:
    explicit RotatingIconLabel(QWidget* parent) : QLabel(parent), timer_(this) {
        timer_.setInterval(60);
        connect(&timer_, &QTimer::timeout, this, [this] {
            angle_ = (angle_ + 18) % 360;
            setProperty("rotationAngle", angle_);
            update();
        });
    }

    void setSpinning(const bool spinning) {
        if (spinning_ == spinning)
            return;
        spinning_ = spinning;
        setProperty("spinning", spinning_);
        if (spinning_)
            timer_.start();
        else {
            timer_.stop();
            angle_ = 0;
            setProperty("rotationAngle", angle_);
        }
        update();
    }

  protected:
    void paintEvent(QPaintEvent* event) override {
        const QPixmap icon = pixmap(Qt::ReturnByValue);
        if (!spinning_ || icon.isNull()) {
            QLabel::paintEvent(event);
            return;
        }
        QPainter painter(this);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.translate(width() / 2.0, height() / 2.0);
        painter.rotate(angle_);
        // The source rect is in physical pixels (the Lucide pixmap is 2× retina), while the
        // destination is in widget coordinates. Using the device-independent size for both
        // sampled only the pixmap's upper-left quadrant and visibly clipped the spinner.
        painter.drawPixmap(QRectF(-width() / 2.0, -height() / 2.0, width(), height()), icon,
                           QRectF(icon.rect()));
    }

  private:
    QTimer timer_;
    int angle_{0};
    bool spinning_{false};
};

// Everything that needs new widgets. Selection and turn status are updated in place.
// The conversation whose message started a running turn; empty once the turn ends.
QString runningAgentSender(const QVariantMap& conversation) {
    return conversation.value(QStringLiteral("running")).toBool()
               ? conversation.value(QStringLiteral("agentTurnNumber")).toString()
               : QString();
}

QStringList structureOf(const QVector<SidebarRepository>& repositories) {
    QStringList structure;
    for (const SidebarRepository& repository : repositories) {
        // The active repository is state, not structure: switching repositories must not rebuild.
        structure << repository.path << repository.name;
        for (const QVariant& value : repository.conversations) {
            const QVariantMap conversation = value.toMap();
            structure << conversation.value(QStringLiteral("id")).toString()
                      << conversation.value(QStringLiteral("path")).toString()
                      << conversation.value(QStringLiteral("title")).toString()
                      << conversation.value(QStringLiteral("number")).toString()
                      << runningAgentSender(conversation);
        }
        structure << QString();
    }
    return structure;
}

RotatingIconLabel* iconLabel(const QByteArray& body, const QString& role, const bool spinning,
                             const int size, QWidget* parent) {
    auto* icon = new RotatingIconLabel(parent);
    icon->setProperty("lucideBody", body);
    icon->setProperty("lucideColorRole", role);
    chrome_geometry::fixedSize(icon, size, size);
    icon->setAttribute(Qt::WA_TransparentForMouseEvents);
    icon->setSpinning(spinning);
    return icon;
}

// Newest rows stay first. Running and selected conversations keep a slot even
// when that pushes the visible set past five.
QSet<QString> previewedConversationIds(const QVariantList& conversations, const bool showAll) {
    QSet<QString> kept;
    if (showAll || conversations.size() <= kPreviewConversationCount) {
        for (const QVariant& value : conversations)
            kept.insert(value.toMap().value(QStringLiteral("id")).toString());
        return kept;
    }
    for (const QVariant& value : conversations) {
        const QVariantMap conversation = value.toMap();
        if (conversation.value(QStringLiteral("running")).toBool() ||
            conversation.value(QStringLiteral("selected")).toBool())
            kept.insert(conversation.value(QStringLiteral("id")).toString());
    }
    for (const QVariant& value : conversations) {
        if (kept.size() >= kPreviewConversationCount)
            break;
        kept.insert(value.toMap().value(QStringLiteral("id")).toString());
    }
    return kept;
}

int compareNames(const QString& left, const QString& right) {
    return QString::localeAwareCompare(left.toCaseFolded(), right.toCaseFolded());
}

qint64 turnStartedAt(const QVariant& conversation) {
    return conversation.toMap().value(QStringLiteral("lastTurnStartedAt")).toLongLong();
}

} // namespace

void sortSidebarRepositories(QVector<SidebarRepository>& repositories) {
    // A repository merges its worktrees' conversations, so each worktree's own recency order is
    // not enough: the conversation whose turn started last leads the whole group.
    for (SidebarRepository& repository : repositories) {
        std::sort(repository.conversations.begin(), repository.conversations.end(),
                  [](const QVariant& left, const QVariant& right) {
                      if (turnStartedAt(left) != turnStartedAt(right))
                          return turnStartedAt(left) > turnStartedAt(right);
                      const QVariantMap leftMap = left.toMap();
                      const QVariantMap rightMap = right.toMap();
                      if (const int titles =
                              compareNames(leftMap.value(QStringLiteral("title")).toString(),
                                           rightMap.value(QStringLiteral("title")).toString());
                          titles != 0)
                          return titles < 0;
                      return leftMap.value(QStringLiteral("id")).toString() <
                             rightMap.value(QStringLiteral("id")).toString();
                  });
    }
    // The first conversation is the newest now, so it dates its repository.
    const auto latest = [](const SidebarRepository& repository) {
        return repository.conversations.isEmpty() ? 0
                                                  : turnStartedAt(repository.conversations.first());
    };
    std::sort(repositories.begin(), repositories.end(),
              [&latest](const SidebarRepository& left, const SidebarRepository& right) {
                  if (left.promoted != right.promoted)
                      return left.promoted;
                  if (latest(left) != latest(right))
                      return latest(left) > latest(right);
                  if (const int names = compareNames(left.name, right.name); names != 0)
                      return names < 0;
                  return left.path < right.path;
              });
}

SidebarRepositoryList::SidebarRepositoryList(QWidget* parent)
    : QWidget(parent), layout_(new QVBoxLayout(this)) {
    chrome_geometry::margins(layout_, 0, 0, 0, 0);
    chrome_geometry::spacing(layout_, 0);
    const QStringList collapsed = QSettings().value(kCollapsedSetting).toStringList();
    collapsedPaths_ = QSet<QString>(collapsed.cbegin(), collapsed.cend());
}

void SidebarRepositoryList::setRepositories(const QVector<SidebarRepository>& projected) {
    QVector<SidebarRepository> repositories = projected;
    sortSidebarRepositories(repositories);
    bool seenChanged = false;
    for (const SidebarRepository& repository : repositories) {
        for (const QVariant& value : repository.conversations) {
            const QVariantMap conversation = value.toMap();
            const QString id = conversation.value(QStringLiteral("id")).toString();
            // Daemon-backed `seen` is authoritative across relaunch; drop stale local marks
            // when a background turn completes and the conversation becomes unread again.
            if (conversation.value(QStringLiteral("seen")).toBool() ||
                conversation.value(QStringLiteral("selected")).toBool()) {
                if (!id.isEmpty() && !seenConversationIds_.contains(id)) {
                    seenConversationIds_.insert(id);
                    seenChanged = true;
                }
            } else if (seenConversationIds_.remove(id)) {
                seenChanged = true;
            }
        }
    }
    // Streaming status updates often repeat the same projection; keep the rows stable.
    if (built_ && repositories == repositories_) {
        if (seenChanged)
            updateRows(repositories_);
        return;
    }
    const QStringList structure = structureOf(repositories);
    repositories_ = repositories;
    if (built_ && structure == structure_) {
        // Selecting a conversation only changes state; rebuilding would flash the sidebar.
        updateRows(repositories);
        applyPresentation();
        return;
    }
    built_ = true;
    structure_ = structure;
    rows_.clear();
    headers_.clear();
    groups_.clear();
    setUpdatesEnabled(false);
    while (auto* item = layout_->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (qsizetype groupIndex = 0; groupIndex < repositories.size(); ++groupIndex) {
        const SidebarRepository& repository = repositories.at(groupIndex);
        if (groupIndex > 0)
            chrome_geometry::addSpacing(layout_, kGroupSpacing);
        auto* group = new QWidget(this);
        group->setObjectName(QStringLiteral("productRepositoryGroup"));
        group->setProperty("repositoryPath", repository.path);
        auto* groupLayout = new QVBoxLayout(group);
        chrome_geometry::margins(groupLayout, 0, 0, 0, 0);
        chrome_geometry::spacing(groupLayout, kConversationSpacing);

        auto* header = new QWidget(group);
        header->setObjectName(QStringLiteral("productRepositoryGroupHeader"));
        chrome_geometry::fixedHeight(header, kHeaderHeight);
        auto* headerLayout = new QHBoxLayout(header);
        chrome_geometry::margins(headerLayout, 0, 0, 2, 0);
        chrome_geometry::spacing(headerLayout, 2);
        auto* name = new QToolButton(header);
        name->setObjectName(QStringLiteral("productRepositoryGroupButton"));
        name->setProperty("kvRole", QStringLiteral("productRepositoryGroup"));
        name->setProperty("activeRepository", repository.active);
        name->setText(
            name->fontMetrics().elidedText(repository.name, Qt::ElideRight, kRepositoryNameWidth));
        name->setToolTip(repository.path);
        name->setAccessibleName(repository.name);
        name->setToolButtonStyle(Qt::ToolButtonTextOnly);
        chrome_geometry::fixedHeight(name, kHeaderHeight);
        connect(name, &QToolButton::clicked, this,
                [this, path = repository.path] { onRepositoryHeaderClicked(path); });
        headerLayout->addWidget(name);
        headerLayout->addStretch();
        header->setProperty("activeRepository", repository.active);
        headers_.append(header);
        {
            auto* create = new QToolButton(header);
            create->setObjectName(QStringLiteral("productNewConversationButton"));
            create->setProperty("kvRole", QStringLiteral("productSectionAction"));
            create->setProperty("lucideBody", QByteArray(lucide::plus));
            create->setToolTip(tr("New conversation in %1").arg(repository.name));
            create->setAccessibleName(create->toolTip());
            chrome_geometry::iconSize(create, QSize(14, 14));
            chrome_geometry::fixedSize(create, 24, 24);
            QSizePolicy policy = create->sizePolicy();
            policy.setRetainSizeWhenHidden(true);
            create->setSizePolicy(policy);
            create->hide();
            connect(create, &QToolButton::clicked, this,
                    [this, path = repository.path] { emit newConversationRequested(path); });
            header->setProperty("hoverAction", QVariant::fromValue<QObject*>(create));
            header->installEventFilter(this);
            headerLayout->addWidget(create);
        }
        groupLayout->addWidget(header);

        for (const QVariant& value : repository.conversations) {
            const QVariantMap conversation = value.toMap();
            const QString id = conversation.value(QStringLiteral("id")).toString();
            const QString worktreePath = conversation.value(QStringLiteral("path")).toString();
            QString title = conversation.value(QStringLiteral("title")).toString();
            if (title.isEmpty())
                title = tr("New conversation");
            const bool seen = conversation.value(QStringLiteral("seen")).toBool() ||
                              seenConversationIds_.contains(id);
            const ConversationPresentation presentation =
                conversationPresentation(conversation, seen);
            const bool selected = conversation.value(QStringLiteral("selected")).toBool();

            auto* button = new QToolButton(group);
            button->setObjectName(QStringLiteral("productConversationButton"));
            button->setProperty("kvRole", QStringLiteral("productConversation"));
            button->setProperty("worktreePath", worktreePath);
            button->setProperty("conversationId", id);
            button->installEventFilter(this);
            button->setCheckable(true);
            button->setChecked(selected);
            button->setProperty("selectedConversation", selected);
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            // The worktree is an attribute of the conversation; name it without a machine path.
            // ADR 0027: agents and the user call a conversation by this number.
            const QString number = conversation.value(QStringLiteral("number")).toString();
            const QString numbered =
                number.isEmpty() ? title : QLatin1Char('#') + number + QLatin1Char(' ') + title;
            const QString sender = runningAgentSender(conversation);
            QString origin;
            if (!sender.isEmpty()) {
                const QString senderTitle =
                    conversation.value(QStringLiteral("agentTurnTitle")).toString();
                origin = (conversation.value(QStringLiteral("agentTurnReply")).toBool()
                              ? tr("Reply from #%1")
                              : tr("Message from #%1"))
                             .arg(sender) +
                         (senderTitle.isEmpty() ? QString() : QStringLiteral(" · ") + senderTitle);
            }
            button->setToolTip(numbered + QLatin1Char('\n') +
                               (origin.isEmpty() ? QString() : origin + QLatin1Char('\n')) +
                               QFileInfo(worktreePath).fileName());
            button->setAccessibleName(numbered);
            button->setAccessibleDescription(origin.isEmpty() ? presentation.label
                                                              : presentation.label +
                                                                    QStringLiteral(". ") + origin);
            button->setToolButtonStyle(Qt::ToolButtonTextOnly);
            chrome_geometry::fixedHeight(button, kConversationHeight);
            button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            auto* row = new QHBoxLayout(button);
            chrome_geometry::margins(row, 8, 0, 8, 0);
            chrome_geometry::spacing(row, 8);
            auto* status =
                iconLabel(presentation.icon, presentation.tone, presentation.spinning, 14, button);
            status->setObjectName(QStringLiteral("productConversationStatusIcon"));
            auto* titleLabel = new ElidedLabel(title, button);
            titleLabel->setProperty("kvRole", QStringLiteral("productConversationTitle"));
            titleLabel->setProperty("selectedConversation", selected);
            titleLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
            row->addWidget(status);
            row->addWidget(titleLabel, 1);
            if (!sender.isEmpty()) {
                auto* senderLabel = new QLabel(tr("from #%1").arg(sender), button);
                senderLabel->setObjectName(QStringLiteral("productConversationAgentSender"));
                senderLabel->setProperty("kvRole",
                                         QStringLiteral("productConversationAgentSender"));
                senderLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
                row->addWidget(senderLabel);
            }
            if (!number.isEmpty()) {
                auto* numberLabel = new QLabel(QLatin1Char('#') + number, button);
                numberLabel->setObjectName(QStringLiteral("productConversationNumber"));
                numberLabel->setProperty("kvRole", QStringLiteral("productConversationNumber"));
                numberLabel->setAttribute(Qt::WA_TransparentForMouseEvents);
                row->addWidget(numberLabel);
            }
            connect(button, &QToolButton::clicked, this, [this, button, worktreePath, id] {
                markConversationSeen(id);
                // Selection follows the session manager; the refresh re-checks the row.
                button->setChecked(button->property("selectedConversation").toBool());
                emit conversationRequested(worktreePath, id);
            });
            connect(button, &QToolButton::customContextMenuRequested, this,
                    [this, worktreePath, id] {
                        emit conversationContextMenuRequested(worktreePath, id);
                    });
            groupLayout->addWidget(button);
            rows_.append(button);
        }
        if (repository.conversations.size() > kPreviewConversationCount) {
            auto* showMore = new QToolButton(group);
            showMore->setObjectName(QStringLiteral("productShowMoreConversations"));
            showMore->setProperty("kvRole", QStringLiteral("productShowMoreConversations"));
            showMore->setProperty("repositoryPath", repository.path);
            showMore->setText(tr("Show more"));
            showMore->setToolButtonStyle(Qt::ToolButtonTextOnly);
            showMore->setStyleSheet(QStringLiteral("text-align: left;"));
            chrome_geometry::fixedHeight(showMore, kShowMoreHeight);
            showMore->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
            connect(showMore, &QToolButton::clicked, this, [this, path = repository.path] {
                showAllPaths_.insert(path);
                applyPresentation();
            });
            groupLayout->addWidget(showMore);
        }
        groups_.append(group);
        layout_->addWidget(group);
    }
    applyPresentation();
    setUpdatesEnabled(true);
}

void SidebarRepositoryList::updateRows(const QVector<SidebarRepository>& repositories) {
    for (qsizetype group = 0; group < repositories.size() && group < headers_.size(); ++group) {
        QWidget* header = headers_.at(group);
        const bool active = repositories.at(group).active;
        header->setProperty("activeRepository", active);
        auto* name =
            header->findChild<QToolButton*>(QStringLiteral("productRepositoryGroupButton"));
        if (name != nullptr && name->property("activeRepository").toBool() != active) {
            name->setProperty("activeRepository", active);
            name->style()->unpolish(name);
            name->style()->polish(name);
        }
    }
    qsizetype row = 0;
    for (const SidebarRepository& repository : repositories) {
        for (const QVariant& value : repository.conversations) {
            if (row >= rows_.size())
                return;
            const QVariantMap conversation = value.toMap();
            const bool selected = conversation.value(QStringLiteral("selected")).toBool();
            const QString id = conversation.value(QStringLiteral("id")).toString();
            const bool seen = conversation.value(QStringLiteral("seen")).toBool() ||
                              seenConversationIds_.contains(id);
            const ConversationPresentation presentation =
                conversationPresentation(conversation, seen);
            QToolButton* button = rows_.at(row++);
            button->setProperty("selectedConversation", selected);
            button->setChecked(selected);
            button->setAccessibleDescription(presentation.label);
            if (auto* status =
                    button->findChild<QLabel*>(QStringLiteral("productConversationStatusIcon"))) {
                status->setProperty("lucideBody", presentation.icon);
                status->setProperty("lucideColorRole", presentation.tone);
                static_cast<RotatingIconLabel*>(status)->setSpinning(presentation.spinning);
            }
            for (auto* title : button->findChildren<QLabel*>()) {
                if (title->property("kvRole").toString() !=
                        QStringLiteral("productConversationTitle") ||
                    title->property("selectedConversation").toBool() == selected)
                    continue;
                title->setProperty("selectedConversation", selected);
                title->style()->unpolish(title);
                title->style()->polish(title);
            }
        }
    }
}

void SidebarRepositoryList::markConversationSeen(const QString& conversationId) {
    if (conversationId.isEmpty() || seenConversationIds_.contains(conversationId))
        return;
    seenConversationIds_.insert(conversationId);
    if (built_)
        updateRows(repositories_);
    emit iconPresentationChanged();
}

void SidebarRepositoryList::applyZoom(const int basePixels) {
    chrome_geometry::apply(this, basePixels);
    applyPresentation();
}

void SidebarRepositoryList::applyPresentation() {
    const auto px = [this](int n) {
        return InterfaceZoomMetrics::geometry(n, chrome_geometry::basePixels(this));
    };
    int height = 0;
    for (qsizetype index = 0; index < groups_.size() && index < repositories_.size(); ++index) {
        if (index > 0)
            height += px(kGroupSpacing);
        QWidget* group = groups_.at(index);
        const SidebarRepository& repository = repositories_.at(index);
        const bool collapsed =
            !repository.conversations.isEmpty() && collapsedPaths_.contains(repository.path);
        const bool showAll = showAllPaths_.contains(repository.path);
        const QSet<QString> preview = previewedConversationIds(repository.conversations, showAll);
        int visibleRows = 0;
        for (auto* button :
             group->findChildren<QToolButton*>(QStringLiteral("productConversationButton"))) {
            const bool visible =
                !collapsed && preview.contains(button->property("conversationId").toString());
            button->setVisible(visible);
            if (visible)
                ++visibleRows;
        }
        auto* showMore =
            group->findChild<QToolButton*>(QStringLiteral("productShowMoreConversations"));
        const bool moreVisible = showMore != nullptr && !collapsed && !showAll &&
                                 repository.conversations.size() > preview.size();
        if (showMore != nullptr)
            showMore->setVisible(moreVisible);
        if (auto* name =
                group->findChild<QToolButton*>(QStringLiteral("productRepositoryGroupButton"))) {
            name->setAccessibleDescription(repository.conversations.isEmpty() ? QString()
                                           : collapsed                        ? tr("Collapsed")
                                                                              : tr("Expanded"));
        }
        const int extra = moreVisible ? px(kShowMoreHeight) + px(kConversationSpacing) : 0;
        group->setFixedHeight(px(kHeaderHeight) +
                              visibleRows * (px(kConversationHeight) + px(kConversationSpacing)) +
                              extra);
        height += group->height();
    }
    setFixedHeight(height);
}

void SidebarRepositoryList::onRepositoryHeaderClicked(const QString& path) {
    const SidebarRepository* repository = nullptr;
    for (const SidebarRepository& candidate : repositories_) {
        if (candidate.path == path) {
            repository = &candidate;
            break;
        }
    }
    if (repository == nullptr || repository->conversations.isEmpty()) {
        emit repositoryRequested(path);
        return;
    }
    if (collapsedPaths_.contains(path)) {
        collapsedPaths_.remove(path);
    } else {
        collapsedPaths_.insert(path);
        showAllPaths_.remove(path);
    }
    persistCollapsedRepositories();
    applyPresentation();
}

void SidebarRepositoryList::persistCollapsedRepositories() const {
    QStringList values(collapsedPaths_.cbegin(), collapsedPaths_.cend());
    values.sort();
    QSettings().setValue(kCollapsedSetting, values);
}

bool SidebarRepositoryList::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::MouseButtonDblClick) {
        const QString conversationId = watched->property("conversationId").toString();
        if (!conversationId.isEmpty()) {
            emit conversationRenameRequested(watched->property("worktreePath").toString(),
                                             conversationId);
            return true;
        }
    }
    if (event->type() == QEvent::Enter || event->type() == QEvent::Leave) {
        // Every repository offers a new conversation; another repository opens first.
        if (auto* action =
                qobject_cast<QWidget*>(watched->property("hoverAction").value<QObject*>()))
            action->setVisible(event->type() == QEvent::Enter);
    }
    return QWidget::eventFilter(watched, event);
}

} // namespace ketplus

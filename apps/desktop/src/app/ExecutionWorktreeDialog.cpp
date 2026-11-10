#include "app/ExecutionWorktreeDialog.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetControlRecipes.h"
#include "KetDialog.h"
#include "KetIcons.h"
#include "KetTextField.h"
#include "KetToast.h"
#include "KetTokens.h"
#include "app/ConversationRepository.h"
#include <QDir>
#include <QFileInfo>
#include <QFrame>
#include <QHBoxLayout>
#include <QHash>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <QtMath>
#include <memory>
#include <utility>

namespace ketplus {
namespace {
constexpr int branchRole = Qt::UserRole + 1;
// A repository's worktree count, once its list has been read.
constexpr int countRole = Qt::UserRole + 2;

// The paginated catalog stays in QListWidget; kit recipes draw only visible rows.
class PickerDelegate final : public QStyledItemDelegate {
  public:
    PickerDelegate(bool repository, QObject* parent)
        : QStyledItemDelegate(parent), repository_(repository) {}
    // As tall as paint() draws: the title line, and the detail line only when there is one, so
    // a repository row is one line as the mock's.
    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override {
        const auto& appearance = ket::Appearance::instance();
        const auto px = [&](qreal value) {
            return ket::Appearance::scale(value, appearance.zoom());
        };
        const auto recipe = ket::treeItemRecipe(false, {}, appearance.theme());
        QFont font = option.font;
        font.setPixelSize(qRound(px(recipe.titlePixels)));
        font.setWeight(QFont::Weight(recipe.titleWeight));
        qreal height = qMax(QFontMetricsF(font).height(), px(recipe.iconSize));
        if (index.data().toString().contains('\n')) {
            font.setPixelSize(qRound(px(recipe.detailPixels)));
            height += px(2) + QFontMetricsF(font).height();
        }
        // paint() insets the row 2 px on each side.
        return {0, qCeil(height + 2 * px(recipe.paddingBlock) + 4)};
    }
    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override {
        const auto& appearance = ket::Appearance::instance();
        const auto px = [&](qreal value) {
            return ket::Appearance::scale(value, appearance.zoom());
        };
        const auto recipe = ket::treeItemRecipe(option.state.testFlag(QStyle::State_Selected),
                                                {option.state.testFlag(QStyle::State_MouseOver),
                                                 false, bool(index.flags() & Qt::ItemIsEnabled),
                                                 option.state.testFlag(QStyle::State_HasFocus)},
                                                appearance.theme());
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setOpacity(recipe.opacity);
        const QRectF area = QRectF(option.rect).adjusted(2, 2, -2, -2);
        painter->setPen(recipe.focusRing.alpha() ? QPen(recipe.focusRing, 2) : QPen(Qt::NoPen));
        painter->setBrush(recipe.background);
        painter->drawRoundedRect(area, px(recipe.radius), px(recipe.radius));
        const qreal icon = px(recipe.iconSize);
        const qreal left = area.left() + px(recipe.paddingInline);
        painter->drawPixmap(QPointF(left, area.top() + px(recipe.paddingBlock)),
                            ket::Icons::pixmap(repository_ ? u"folder" : u"branch", icon,
                                               painter->device()->devicePixelRatioF(),
                                               recipe.icon));
        const auto lines = index.data().toString().split('\n');
        const qreal textLeft = left + icon + px(recipe.spacing);
        qreal right = area.right() - px(recipe.paddingInline);
        QFont font = option.font;
        // A repository's worktree count, on the right as the mock's `.tree-count`.
        if (const auto count = index.data(countRole).toString(); !count.isEmpty()) {
            font.setPixelSize(qRound(px(recipe.countPixels)));
            painter->setFont(font);
            painter->setPen(recipe.count);
            const qreal countWidth = QFontMetricsF(font).horizontalAdvance(count);
            painter->drawText(QRectF(right - countWidth, area.top() + px(recipe.paddingBlock),
                                     countWidth, QFontMetricsF(font).height()),
                              Qt::AlignVCenter | Qt::AlignRight, count);
            right -= countWidth + px(recipe.spacing);
        }
        const qreal width = qMax(0.0, right - textLeft);
        font.setPixelSize(qRound(px(recipe.titlePixels)));
        font.setWeight(QFont::Weight(recipe.titleWeight));
        painter->setFont(font);
        painter->setPen(recipe.title);
        const qreal top = area.top() + px(recipe.paddingBlock);
        const qreal titleHeight = QFontMetricsF(font).height();
        painter->drawText(
            QRectF(textLeft, top, width, titleHeight), Qt::AlignVCenter,
            QFontMetrics(font).elidedText(lines.value(0), Qt::ElideRight, qRound(width)));
        font.setPixelSize(qRound(px(recipe.detailPixels)));
        font.setWeight(QFont::Normal);
        painter->setFont(font);
        painter->setPen(recipe.detail);
        painter->drawText(
            QRectF(textLeft, top + titleHeight + px(2), width, QFontMetricsF(font).height()),
            Qt::AlignVCenter,
            QFontMetrics(font).elidedText(lines.value(1), Qt::ElideMiddle, qRound(width)));
        painter->restore();
    }

  private:
    bool repository_;
};
// The mock's 1 px panel-border rule between the columns, in the current theme.
class ColumnRule final : public QWidget {
  public:
    explicit ColumnRule(QWidget* parent) : QWidget(parent) {
        setObjectName("repositoryPickerDivider");
        setFixedWidth(1);
        connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
                qOverload<>(&QWidget::update));
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter(this).fillRect(
            rect(), ket::Tokens::color(u"panel-border", ket::Appearance::instance().theme()));
    }
};
} // namespace

void showExecutionWorktreeDialog(QWidget* parent, ExecutionWorktreeRequest request,
                                 std::function<void(const QString&)> selected,
                                 const QString& fileContextPath, const QStringList& repositories,
                                 const QString& currentRepository,
                                 std::function<void()> createWorktree) {
    // The mock's Repository & worktree picker: choose a repository, then one of its worktrees,
    // and "Use this repository" moves the conversation's next turn there (ADR 0105).
    // An in-window card as the mock's KetDialog (`width: px(800)`): title and close button in
    // its header, the two columns in its body, the actions in its footer.
    auto* dialog = new ket::Dialog("Repository & worktree", parent);
    dialog->setObjectName("executionWorktreeDialog");
    dialog->setCardWidth(800);
    QObject::connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    auto* body = new QWidget;
    body->setMinimumHeight(320);
    auto* layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);
    auto* columns = new QHBoxLayout;
    columns->setSpacing(12);
    auto* repoColumn = new QVBoxLayout;
    repoColumn->setSpacing(8);
    auto* treeColumn = new QVBoxLayout;
    treeColumn->setSpacing(8);
    auto* repoPane = new QWidget(body);
    repoPane->setFixedWidth(222);
    repoPane->setLayout(repoColumn);
    repoColumn->setContentsMargins(0, 0, 0, 0);
    columns->addWidget(repoPane);
    auto* divider = new ColumnRule(body);
    columns->addWidget(divider);
    columns->addLayout(treeColumn, 1);
    layout->addLayout(columns, 1);
    auto* repoSearch = new ket::TextField(body);
    repoSearch->setLeadingIcon(ket::Icons::codeForLucide(u"search"));
    repoSearch->setPlaceholderText("Search repositories…");
    repoSearch->setAccessibleName("Search repositories");
    repoSearch->setObjectName("repositorySearch");
    repoColumn->addWidget(repoSearch);
    auto* repos = new QListWidget(body);
    repos->setObjectName("executionRepositoryList");
    repos->setAccessibleName("Conversation repositories");
    repos->setFrameShape(QFrame::NoFrame);
    repos->setStyleSheet(QStringLiteral("QListWidget { background: transparent; }"));
    repos->setItemDelegate(new PickerDelegate(true, repos));
    repoColumn->addWidget(repos, 1);
    auto* search = new ket::TextField(body);
    search->setLeadingIcon(ket::Icons::codeForLucide(u"search"));
    search->setPlaceholderText("Search branch or folder…");
    search->setAccessibleName("Search worktrees");
    search->setObjectName("worktreeSearch");
    treeColumn->addWidget(search);
    auto* list = new QListWidget(body);
    list->setObjectName("executionWorktreeList");
    list->setAccessibleName("Execution worktrees");
    list->setFrameShape(QFrame::NoFrame);
    list->setStyleSheet(QStringLiteral("QListWidget { background: transparent; }"));
    list->setUniformItemSizes(true);
    list->setSpacing(4);
    list->setItemDelegate(new PickerDelegate(false, list));
    treeColumn->addWidget(list, 1);
    auto* status = new QLabel(body);
    status->setObjectName("worktreeStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    dialog->setBody(body);
    auto* create = dialog->addButton("Create worktree…", ket::ButtonVariant::Ghost);
    create->setObjectName("createWorktreeButton");
    create->setIcon(ket::Icons::icon(u"plus"));
    auto* use = dialog->addButton("Use this repository", ket::ButtonVariant::Primary);
    use->setObjectName("useRepositoryButton");
    use->setEnabled(false);
    create->setVisible(static_cast<bool>(createWorktree));
    struct State {
        int offset{0}, total{0}, epoch{0};
        qint64 revision{0};
        // pending: the search changed and its page has not arrived yet.
        bool loading{false}, switching{false}, available{false}, pending{false};
        // keep: the worktree to choose again after a reload; notice: shown once it lands.
        QString keep, notice;
        // The conversation's worktree and its repository; the repository whose worktrees
        // are listed; the worktree chosen in that list.
        QString current, repository, listed, chosen;
        QStringList roots;
        QHash<QString, QString> lastWorktrees;
        QHash<QString, int> counts;
    };
    auto state = std::make_shared<State>();
    state->current = fileContextPath;
    state->repository = currentRepository;
    state->listed = currentRepository;
    state->roots = repositories;
    if (!currentRepository.isEmpty() && !state->roots.contains(currentRepository))
        state->roots.prepend(currentRepository);
    auto renderRepos = [=] {
        const QSignalBlocker blocker(repos);
        repos->clear();
        auto roots = state->roots;
        roots.removeAll(state->repository);
        if (!state->repository.isEmpty())
            roots.prepend(state->repository);
        for (const auto& root : roots) {
            if (!(QFileInfo(root).fileName() + " " + root)
                     .contains(repoSearch->text(), Qt::CaseInsensitive))
                continue;
            auto* row = new QListWidgetItem(QFileInfo(root).fileName(), repos);
            row->setData(Qt::UserRole, root);
            if (state->counts.contains(root))
                row->setData(countRole, QString::number(state->counts.value(root)));
            row->setToolTip(root);
            if (root == state->listed)
                repos->setCurrentItem(row);
        }
    };
    auto updateUse = [=] {
        use->setEnabled(state->available && !state->switching && !state->loading &&
                        !state->pending && !state->chosen.isEmpty());
    };
    QPointer<ket::Dialog> guard(dialog);
    auto load = [=] {
        if (state->loading || state->switching)
            return;
        state->loading = true;
        updateUse();
        const auto epoch = ++state->epoch;
        status->setText("Loading worktrees…");
        if (state->offset == 0)
            list->setEnabled(false);
        QJsonObject payload{{"query", search->text()}, {"offset", state->offset}, {"limit", 100}};
        // Another repository's worktrees are listed by path, over local IPC only.
        const bool foreign = !state->listed.isEmpty() && state->listed != state->repository;
        if (foreign)
            payload.insert("repository_path", state->listed);
        request("session.worktrees", payload, [=](const QJsonObject& result, const QString& error) {
            if (!guard || state->epoch != epoch)
                return;
            state->loading = false;
            state->pending = false;
            if (!error.isEmpty() || !result.value("error_code").toString().isEmpty()) {
                status->setText(error.isEmpty() ? result.value("error_message").toString() : error);
                // Rows from another repository or query must not stay choosable.
                if (state->offset == 0) {
                    list->clear();
                    state->chosen.clear();
                }
                list->setEnabled(true);
                updateUse();
                return;
            }
            // A service from before ADR 0105 ignores repository_path and would list the
            // conversation's own repository under another name.
            if (foreign && !result.contains("repository_path")) {
                list->clear();
                state->chosen.clear();
                status->setText("Restart KétPlus to list another repository's worktrees.");
                list->setEnabled(true);
                updateUse();
                return;
            }
            state->current = result.value("worktree_path").toString();
            for (const auto& root : result.value("repositories").toArray()) {
                const auto path = root.toString();
                if (!path.isEmpty() && !state->roots.contains(path))
                    state->roots.append(path);
            }
            if (QFileInfo(QDir(state->current).filePath(".git")).exists())
                state->repository = conversationRepositoryRoot(state->current);
            if (state->listed.isEmpty())
                state->listed = state->repository;
            state->revision = result.value("context_revision").toInteger();
            for (const auto& context : result.value("contexts").toArray()) {
                const auto path = context.toObject().value("worktree_path").toString();
                const auto owner = conversationRepositoryRoot(path);
                if (state->roots.contains(owner))
                    state->lastWorktrees[owner] = path;
            }
            state->available = result.value("switch_supported").toBool() &&
                               result.value("switch_available").toBool(true);
            const bool reset = state->offset == 0;
            if (reset) {
                list->clear();
                state->chosen.clear();
            }
            const auto preferred = !state->keep.isEmpty() ? state->keep
                                   : state->listed == state->repository
                                       ? state->current
                                       : state->lastWorktrees.value(state->listed);
            state->keep.clear();
            for (const auto& value : result.value("worktrees").toArray()) {
                const auto entry = value.toObject();
                const auto path = entry.value("worktree_path").toString();
                const auto branch = entry.value("branch").toString();
                auto* row = new QListWidgetItem(
                    branch + (entry.value("main").toBool() ? "  · Main" : "") +
                        (entry.value("model_touched").toBool() ? " ✦" : "") +
                        (path == state->current ? " · Current" : "") + "\n" + path,
                    list);
                row->setData(Qt::UserRole, path);
                row->setData(branchRole, branch);
                row->setToolTip(path);
                if (path == preferred || (reset && list->count() == 1)) {
                    list->setCurrentItem(row);
                    state->chosen = path;
                }
            }
            state->total = result.value("total").toInt();
            state->offset += result.value("worktrees").toArray().size();
            if (search->text().isEmpty())
                state->counts[state->listed] = state->total;
            renderRepos();
            status->setText(!state->available   ? "Switching is unavailable for this provider or "
                                                  "while a request is pending."
                            : state->total == 0 ? "No worktree matches."
                                                : std::exchange(state->notice, {}));
            list->setEnabled(state->available);
            repos->setEnabled(state->available);
            updateUse();
            if (reset)
                list->scrollToTop();
            if (result.value("worktrees").toArray().isEmpty())
                state->total = state->offset;
        });
    };
    auto reload = [=] {
        ++state->epoch;
        state->loading = false;
        state->offset = state->total = 0;
        load();
    };
    auto change = [=](const QString& path, const QString& repository, const QString& branch) {
        if (state->switching || !state->available)
            return;
        if (path == state->current) {
            dialog->accept();
            return;
        }
        state->switching = true;
        ++state->epoch;
        state->loading = false;
        list->setEnabled(false);
        repos->setEnabled(false);
        updateUse();
        status->setText("Switching conversation…");
        request(
            "session.worktree.switch",
            {{"worktree_path", path}, {"expected_context_revision", state->revision}},
            [=](const QJsonObject& result, const QString& error) {
                if (!guard)
                    return;
                state->switching = false;
                // Another window changed the context: read it again, keep this choice, and
                // let Use send the new revision.
                if (error.isEmpty() &&
                    result.value("error_code").toString() == QStringLiteral("stale_context")) {
                    state->keep = path;
                    state->notice = "The conversation changed elsewhere. Check the worktree, "
                                    "then choose Use this repository again.";
                    list->setEnabled(state->available);
                    repos->setEnabled(state->available);
                    reload();
                    return;
                }
                if (!error.isEmpty() || !result.value("ok").toBool()) {
                    status->setText(
                        error.isEmpty()
                            ? result.value("error_message")
                                  .toString(
                                      "The conversation could not be changed. Refresh and retry.")
                            : error);
                    list->setEnabled(state->available);
                    repos->setEnabled(state->available);
                    updateUse();
                    return;
                }
                state->current = result.value("worktree_path").toString(path);
                state->repository = repository;
                selected(state->current);
                if (parent != nullptr)
                    ket::Toast::showMessage(parent,
                                            QStringLiteral("Next turn: %1 · %2")
                                                .arg(QFileInfo(repository).fileName(), branch));
                dialog->accept();
            });
    };
    auto* debounce = new QTimer(dialog);
    debounce->setSingleShot(true);
    debounce->setInterval(250);
    QObject::connect(debounce, &QTimer::timeout, dialog, load);
    QObject::connect(search, &QLineEdit::textChanged, dialog, [=] {
        ++state->epoch;
        state->offset = state->total = 0;
        state->loading = false;
        state->pending = true;
        list->setEnabled(false);
        updateUse();
        debounce->start();
    });
    QObject::connect(repoSearch, &QLineEdit::textChanged, dialog, renderRepos);
    const auto listRepository = [=](QListWidgetItem* row) {
        const auto root = row->data(Qt::UserRole).toString();
        if (root == state->listed || state->switching)
            return;
        state->listed = root;
        list->clear();
        state->chosen.clear();
        reload();
    };
    QObject::connect(repos, &QListWidget::itemClicked, dialog, listRepository);
    QObject::connect(repos, &QListWidget::itemActivated, dialog, listRepository);
    // Arrow keys move the current repository as a click does.
    QObject::connect(repos, &QListWidget::currentItemChanged, dialog,
                     [=](QListWidgetItem* row, QListWidgetItem*) {
                         if (row != nullptr)
                             listRepository(row);
                     });
    QObject::connect(list->verticalScrollBar(), &QScrollBar::valueChanged, dialog, [=](int value) {
        if (value >= list->verticalScrollBar()->maximum() - 100 && state->offset < state->total)
            load();
    });
    const auto choose = [=](QListWidgetItem* row) {
        state->chosen = row != nullptr ? row->data(Qt::UserRole).toString() : QString{};
        updateUse();
    };
    const auto confirm = [=] {
        for (int index = 0; index < list->count(); ++index) {
            auto* row = list->item(index);
            if (row->data(Qt::UserRole).toString() == state->chosen) {
                change(state->chosen, state->listed, row->data(branchRole).toString());
                return;
            }
        }
    };
    // The current row is the choice, whether a click or the arrow keys moved it.
    QObject::connect(list, &QListWidget::currentItemChanged, dialog,
                     [=](QListWidgetItem* row, QListWidgetItem*) { choose(row); });
    QObject::connect(list, &QListWidget::itemActivated, dialog, [=](QListWidgetItem* row) {
        choose(row);
        confirm();
    });
    QObject::connect(use, &QAbstractButton::clicked, dialog, confirm);
    QObject::connect(create, &QAbstractButton::clicked, dialog, [dialog, createWorktree] {
        dialog->reject();
        if (createWorktree)
            createWorktree();
    });
    dialog->open();
    renderRepos();
    load();
}
} // namespace ketplus

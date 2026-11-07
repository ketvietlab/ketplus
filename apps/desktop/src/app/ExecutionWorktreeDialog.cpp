#include "app/ExecutionWorktreeDialog.h"
#include "KetAppearance.h"
#include "KetButton.h"
#include "KetControlRecipes.h"
#include "KetIcons.h"
#include "KetTextField.h"
#include "app/ConversationRepository.h"
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
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
#include <QStyledItemDelegate>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

namespace ketplus {
namespace {
// The paginated catalog stays in QListWidget; kit recipes draw only visible rows.
class PickerDelegate final : public QStyledItemDelegate {
  public:
    PickerDelegate(bool repository, QObject* parent)
        : QStyledItemDelegate(parent), repository_(repository) {}
    QSize sizeHint(const QStyleOptionViewItem&, const QModelIndex&) const override {
        return {0, qRound(ket::Appearance::scale(52, ket::Appearance::instance().zoom()))};
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
        const qreal width = qMax(0.0, area.right() - textLeft - px(recipe.paddingInline));
        QFont font = option.font;
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
} // namespace

void showExecutionWorktreeDialog(QWidget* parent, ExecutionWorktreeRequest request,
                                 std::function<void(const QString&)> selected,
                                 const QString& fileContextPath, const QStringList& repositories,
                                 const QString& currentRepository) {
    auto* dialog = new QDialog(parent);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Conversation repository & worktree");
    dialog->setObjectName("executionWorktreeDialog");
    dialog->resize(800, 580);
    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    auto* note = new QLabel("Choose where this conversation continues. A running turn keeps its "
                            "original worktree; your choice applies to the next turn.",
                            dialog);
    note->setWordWrap(true);
    layout->addWidget(note);
    auto* current = new QLabel(dialog);
    current->setWordWrap(true);
    current->setAccessibleName("Current conversation worktree");
    layout->addWidget(current);
    auto* columns = new QHBoxLayout;
    auto* repoColumn = new QVBoxLayout;
    auto* treeColumn = new QVBoxLayout;
    columns->addLayout(repoColumn, 2);
    columns->addLayout(treeColumn, 3);
    layout->addLayout(columns, 1);
    repoColumn->addWidget(new QLabel("Repository", dialog));
    auto* repoSearch = new ket::TextField(dialog);
    repoSearch->setLeadingIcon(QStringLiteral("search"));
    repoSearch->setPlaceholderText("Search repositories");
    repoSearch->setAccessibleName("Search repositories");
    repoSearch->setObjectName("repositorySearch");
    repoColumn->addWidget(repoSearch);
    auto* repos = new QListWidget(dialog);
    repos->setObjectName("executionRepositoryList");
    repos->setAccessibleName("Conversation repositories");
    repos->setItemDelegate(new PickerDelegate(true, repos));
    repoColumn->addWidget(repos, 1);
    auto* count = new QLabel("Worktree", dialog);
    treeColumn->addWidget(count);
    auto* search = new ket::TextField(dialog);
    search->setLeadingIcon(QStringLiteral("search"));
    search->setPlaceholderText("Search branch or folder");
    search->setAccessibleName("Search worktrees");
    search->setObjectName("worktreeSearch");
    treeColumn->addWidget(search);
    auto* list = new QListWidget(dialog);
    list->setObjectName("executionWorktreeList");
    list->setAccessibleName("Execution worktrees");
    list->setUniformItemSizes(true);
    list->setSpacing(4);
    list->setItemDelegate(new PickerDelegate(false, list));
    treeColumn->addWidget(list, 1);
    auto* status = new QLabel(dialog);
    status->setObjectName("worktreeStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* source = new QLabel(
        "History and drafts stay here. Earlier file links keep their source worktree.", dialog);
    source->setWordWrap(true);
    layout->addWidget(source);
    auto* buttons = new QDialogButtonBox(dialog);
    auto* close = new ket::Button("Close", ket::ButtonVariant::Secondary, dialog);
    auto* refresh = new ket::Button("Refresh", ket::ButtonVariant::Ghost, dialog);
    auto* retry = new ket::Button("Retry", ket::ButtonVariant::Primary, dialog);
    buttons->addButton(close, QDialogButtonBox::RejectRole);
    buttons->addButton(refresh, QDialogButtonBox::ActionRole);
    buttons->addButton(retry, QDialogButtonBox::ActionRole);
    retry->hide();
    layout->addWidget(buttons);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    struct State {
        int offset{0}, total{0}, epoch{0};
        qint64 revision{0};
        bool loading{false}, switching{false}, available{false};
        QString current, repository, retryPath, retryRepository;
        QStringList roots;
        QHash<QString, QString> lastWorktrees;
    };
    auto state = std::make_shared<State>();
    state->current = fileContextPath;
    state->repository = currentRepository;
    state->roots = repositories;
    if (!currentRepository.isEmpty() && !state->roots.contains(currentRepository))
        state->roots.prepend(currentRepository);
    auto renderRepos = [=] {
        repos->clear();
        auto roots = state->roots;
        roots.removeAll(state->repository);
        if (!state->repository.isEmpty())
            roots.prepend(state->repository);
        for (const auto& root : roots) {
            if (!(QFileInfo(root).fileName() + " " + root)
                     .contains(repoSearch->text(), Qt::CaseInsensitive))
                continue;
            auto* row = new QListWidgetItem(QFileInfo(root).fileName() +
                                                (root == state->repository ? " · Current" : ""),
                                            repos);
            row->setData(Qt::UserRole, root);
            row->setToolTip(root);

            if (root == state->repository)
                repos->setCurrentItem(row);
        }
    };
    QPointer<QDialog> guard(dialog);
    auto load = [=] {
        if (state->loading || state->switching)
            return;
        state->loading = true;
        const auto epoch = ++state->epoch;
        status->setText("Loading worktrees…");
        if (state->offset == 0)
            list->setEnabled(false);
        request("session.worktrees",
                {{"query", search->text()}, {"offset", state->offset}, {"limit", 100}},
                [=](const QJsonObject& result, const QString& error) {
                    if (!guard || state->epoch != epoch)
                        return;
                    state->loading = false;
                    if (!error.isEmpty() || !result.value("error_code").toString().isEmpty()) {
                        status->setText(error.isEmpty() ? result.value("error_message").toString()
                                                        : error);
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
                    renderRepos();
                    state->revision = result.value("context_revision").toInteger();
                    for (const auto& context : result.value("contexts").toArray()) {
                        const auto path = context.toObject().value("worktree_path").toString();
                        const auto owner = conversationRepositoryRoot(path);
                        if (state->roots.contains(owner))
                            state->lastWorktrees[owner] = path;
                    }
                    state->available = result.value("switch_supported").toBool() &&
                                       result.value("switch_available").toBool(true);
                    current->setText("Current · " + state->current);
                    const bool reset = state->offset == 0;
                    if (reset)
                        list->clear();
                    for (const auto& value : result.value("worktrees").toArray()) {
                        const auto entry = value.toObject();
                        const auto path = entry.value("worktree_path").toString();
                        auto* row = new QListWidgetItem(
                            entry.value("branch").toString() +
                                (entry.value("model_touched").toBool() ? " ✦" : "") +
                                (path == state->current ? " · Current" : "") + "\n" +
                                QFileInfo(path).fileName(),
                            list);
                        row->setData(Qt::UserRole, path);
                        row->setToolTip(path);
                    }
                    state->total = result.value("total").toInt();
                    state->offset += result.value("worktrees").toArray().size();
                    count->setText(QString("Worktree · %1 · Model-used first").arg(state->total));
                    status->setText(!state->available   ? "Switching is unavailable for this "
                                                          "provider or while a request is pending."
                                    : state->total == 0 ? "No worktrees match."
                                                        : "");
                    list->setEnabled(state->available);
                    repos->setEnabled(state->available);
                    if (reset)
                        list->scrollToTop();
                    if (result.value("worktrees").toArray().isEmpty())
                        state->total = state->offset;
                });
    };
    auto change = [=](const QString& path, const QString& repository) {
        if (state->switching || !state->available)
            return;
        if (path == state->current)
            return;
        state->switching = true;
        ++state->epoch;
        state->loading = false;
        state->retryPath = path;
        state->retryRepository = repository;
        retry->hide();
        list->setEnabled(false);
        repos->setEnabled(false);
        refresh->setEnabled(false);
        status->setText("Switching conversation…");
        request(
            "session.worktree.switch",
            {{"worktree_path", path}, {"expected_context_revision", state->revision}},
            [=](const QJsonObject& result, const QString& error) {
                if (!guard)
                    return;
                state->switching = false;
                refresh->setEnabled(true);
                if (!error.isEmpty() || !result.value("ok").toBool()) {
                    status->setText(
                        error.isEmpty()
                            ? result.value("error_message")
                                  .toString(
                                      "The conversation could not be changed. Refresh and retry.")
                            : error);
                    list->setEnabled(state->available);
                    repos->setEnabled(state->available);
                    renderRepos();
                    retry->show();
                    return;
                }
                state->current = result.value("worktree_path").toString(path);
                state->revision = result.value("context_revision").toInteger(state->revision);
                state->repository = repository;
                state->lastWorktrees[repository] = state->current;
                state->offset = state->total = 0;
                selected(state->current);
                renderRepos();
                load();
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
        list->setEnabled(false);
        debounce->start();
    });
    QObject::connect(repoSearch, &QLineEdit::textChanged, dialog, renderRepos);
    QObject::connect(repos, &QListWidget::itemClicked, dialog, [=](QListWidgetItem* row) {
        const auto root = row->data(Qt::UserRole).toString();
        if (root != state->repository)
            change(QFileInfo(state->lastWorktrees.value(root)).isDir()
                       ? state->lastWorktrees.value(root)
                       : root,
                   root);
    });
    QObject::connect(repos, &QListWidget::itemActivated, dialog, [=](QListWidgetItem* row) {
        const auto root = row->data(Qt::UserRole).toString();
        if (root != state->repository)
            change(QFileInfo(state->lastWorktrees.value(root)).isDir()
                       ? state->lastWorktrees.value(root)
                       : root,
                   root);
    });
    QObject::connect(list->verticalScrollBar(), &QScrollBar::valueChanged, dialog, [=](int value) {
        if (value >= list->verticalScrollBar()->maximum() - 100 && state->offset < state->total)
            load();
    });
    QObject::connect(refresh, &QAbstractButton::clicked, dialog, [=] {
        ++state->epoch;
        state->loading = false;
        state->offset = state->total = 0;
        load();
    });
    QObject::connect(retry, &QAbstractButton::clicked, dialog,
                     [=] { change(state->retryPath, state->retryRepository); });
    const auto select = [=](QListWidgetItem* row) {
        change(row->data(Qt::UserRole).toString(), state->repository);
    };
    QObject::connect(list, &QListWidget::itemClicked, dialog, select);
    QObject::connect(list, &QListWidget::itemActivated, dialog, select);
    dialog->open();
    renderRepos();
    load();
}
} // namespace ketplus

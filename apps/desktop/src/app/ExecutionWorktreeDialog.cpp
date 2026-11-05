#include "app/ExecutionWorktreeDialog.h"
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
#include <QPointer>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>
#include <QVBoxLayout>
#include <memory>

namespace ketplus {
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
    auto* repoSearch = new QLineEdit(dialog);
    repoSearch->setPlaceholderText("Search repositories");
    repoSearch->setAccessibleName("Search repositories");
    repoSearch->setObjectName("repositorySearch");
    repoColumn->addWidget(repoSearch);
    auto* repos = new QListWidget(dialog);
    repos->setObjectName("executionRepositoryList");
    repos->setAccessibleName("Conversation repositories");
    repoColumn->addWidget(repos, 1);
    auto* count = new QLabel("Worktree", dialog);
    treeColumn->addWidget(count);
    auto* search = new QLineEdit(dialog);
    search->setPlaceholderText("Search branch or folder");
    search->setAccessibleName("Search worktrees");
    search->setObjectName("worktreeSearch");
    treeColumn->addWidget(search);
    auto* list = new QListWidget(dialog);
    list->setObjectName("executionWorktreeList");
    list->setAccessibleName("Execution worktrees");
    list->setUniformItemSizes(true);
    list->setSpacing(4);
    list->setStyleSheet("QListWidget::item { padding: 8px 12px; }");
    treeColumn->addWidget(list, 1);
    auto* status = new QLabel(dialog);
    status->setObjectName("worktreeStatus");
    status->setWordWrap(true);
    layout->addWidget(status);
    auto* source = new QLabel(
        "History and drafts stay here. Earlier file links keep their source worktree.", dialog);
    source->setWordWrap(true);
    layout->addWidget(source);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* refresh = buttons->addButton("Refresh", QDialogButtonBox::ActionRole);
    auto* retry = buttons->addButton("Retry", QDialogButtonBox::ActionRole);
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
            row->setSizeHint(QSize(0, 48));
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
                        row->setSizeHint(QSize(0, 58));
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
    QObject::connect(refresh, &QPushButton::clicked, dialog, [=] {
        ++state->epoch;
        state->loading = false;
        state->offset = state->total = 0;
        load();
    });
    QObject::connect(retry, &QPushButton::clicked, dialog,
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

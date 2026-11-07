#include "app/AgentWorktreeDialog.h"
#include "KetAppearance.h"
#include "KetRecipes.h"
#include "KetSelect.h"
#include "KetTextField.h"

#include "app/AgentWorktreeCreator.h"

#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCompleter>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QProcess>
#include <QProcessEnvironment>
#include <QPushButton>
#include <QRadioButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QStyleOptionButton>
#include <QVBoxLayout>
#include <QtConcurrentRun>

#include <algorithm>
#include <utility>

namespace ketplus {
namespace {
// Native macOS radio indicators can retain a near-black outline under our dark palette.
// Keep the native label/focus treatment and draw only the indicator from semantic colours.
class WorktreeRadioButton final : public QRadioButton {
  public:
    using QRadioButton::QRadioButton;

  protected:
    void paintEvent(QPaintEvent* event) override {
        QRadioButton::paintEvent(event);
        QStyleOptionButton option;
        initStyleOption(&option);
        const QRectF indicator =
            style()->subElementRect(QStyle::SE_RadioButtonIndicator, &option, this);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const auto group = isEnabled() ? QPalette::Active : QPalette::Disabled;
        const QColor accent = palette().color(group, QPalette::Text);
        QColor outline = palette().color(group, QPalette::Text);
        outline.setAlphaF(0.65);
        painter.setPen(QPen(isChecked() ? accent : outline, 1.5));
        painter.setBrush(palette().color(QPalette::Window));
        painter.drawEllipse(indicator.adjusted(1, 1, -1, -1));
        if (isChecked()) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(accent);
            const qreal inset = indicator.width() * 0.3;
            painter.drawEllipse(indicator.adjusted(inset, inset, -inset, -inset));
        }
    }
};

struct BranchList {
    QByteArray output;
    bool success{false};
};
BranchList loadBranches(const QString& root) {
    QProcess git;
    auto env = QProcessEnvironment::systemEnvironment();
    env.insert("GIT_TERMINAL_PROMPT", "0");
    git.setProcessEnvironment(env);
    git.setWorkingDirectory(root);
    git.start("git", {"for-each-ref", "--sort=refname", "--format=%(refname)%09%(HEAD)%09%(symref)",
                      "refs/heads", "refs/remotes"});
    BranchList result;
    if (!git.waitForStarted(3000))
        return result;
    // Read incrementally so repositories with many refs cannot grow output without bound.
    for (int elapsed = 0; elapsed < 10000; elapsed += 100) {
        git.waitForFinished(100);
        result.output += git.readAllStandardOutput();
        if (result.output.size() > 1024 * 1024)
            break;
        if (git.state() == QProcess::NotRunning) {
            result.success = git.exitStatus() == QProcess::NormalExit && git.exitCode() == 0;
            return result;
        }
    }
    git.kill();
    git.waitForFinished(1000);
    return {};
}

QString worktreeLabel(const GitWorktree& worktree) {
    QString name = worktree.branch;
    if (name.isEmpty())
        name = QObject::tr("Detached %1").arg(worktree.head.left(8));
    const QString directoryName = QFileInfo(worktree.path).fileName();
    if (!directoryName.isEmpty() && directoryName != name)
        name.append(QStringLiteral(" — %1").arg(directoryName));
    return name;
}

QString canonicalPath(const QString& path) {
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath() : canonical);
}

} // namespace

AgentWorktreeDialog::AgentWorktreeDialog(const QString& repositoryPath, QWidget* parent,
                                         const Purpose purpose, QString primaryWorktreePath,
                                         QVector<GitWorktree> worktrees)
    : QDialog(parent), purpose_(purpose),
      primaryWorktreePath_(primaryWorktreePath.isEmpty() ? repositoryPath
                                                         : std::move(primaryWorktreePath)),
      branch_(new ket::TextField(this)), base_(new ket::Select(this)), error_(new QLabel(this)),
      status_(new QLabel(this)) {
    const bool conversation = purpose_ == Purpose::Conversation;
    setObjectName(conversation ? QStringLiteral("agentConversationDialog")
                               : QStringLiteral("agentWorktreeDialog"));
    setWindowTitle(conversation ? tr("New conversation") : tr("Create Worktree"));
    setMinimumWidth(conversation ? 560 : 420);
    branch_->setObjectName("worktreeBranchName");
    error_->setObjectName(QStringLiteral("worktreeDialogError"));
    error_->setWordWrap(true);
    const auto applyErrorColor = [this] {
        auto colors = error_->palette();
        colors.setColor(
            QPalette::WindowText,
            ket::badgeRecipe(ket::BadgeTone::Danger, ket::Appearance::instance().theme()).text);
        error_->setPalette(colors);
    };
    applyErrorColor();
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, error_, applyErrorColor);
    error_->hide();
    status_->setWordWrap(true);

    base_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    base_->setMinimumContentsLength(40);
    base_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    buttons->setObjectName(QStringLiteral("worktreeDialogButtons"));
    buttons->button(QDialogButtonBox::Ok)->setText(tr("Create"));
    buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
    if (conversation) {
        // The label follows the chosen section, so the button is sized for the
        // wider of the two up front and never reflows under the pointer.
        auto* ok = buttons->button(QDialogButtonBox::Ok);
        const QFontMetrics metrics(ok->font());
        ok->setMinimumWidth(
            std::max(ok->sizeHint().width(), std::max(metrics.horizontalAdvance(tr("Create")),
                                                      metrics.horizontalAdvance(tr("Start"))) +
                                                 32));
    }

    if (conversation) {
        // Two separate jobs, so two separate sections: running the conversation in
        // a checkout that already exists, and cutting a new branch to run it in.
        // A worktree is a checkout of a branch, so only the second names a branch.
        auto* layout = new QVBoxLayout(this);
        existingWorktree_ = new WorktreeRadioButton(tr("Work in an existing worktree"), this);
        existingWorktree_->setObjectName(QStringLiteral("useExistingWorktreeRadio"));
        newWorktree_ = new WorktreeRadioButton(tr("Create a new worktree"), this);
        newWorktree_->setObjectName(QStringLiteral("createNewWorktreeRadio"));
        // The two radios live in separate group boxes, which would make them
        // separately exclusive; one group keeps the choice a single decision.
        auto* section = new QButtonGroup(this);
        section->addButton(existingWorktree_);
        section->addButton(newWorktree_);
        existingWorktree_->setChecked(true);

        auto* existingBox = new QGroupBox(this);
        existingBox->setObjectName(QStringLiteral("existingWorktreeSection"));
        auto* existingLayout = new QVBoxLayout(existingBox);
        existingLayout->addWidget(existingWorktree_);
        auto* existingHelp =
            new QLabel(tr("The conversation runs in a checkout you already have. Nothing is "
                          "created and no branch is changed."),
                       existingBox);
        existingHelp->setWordWrap(true);
        existingHelp->setEnabled(false);
        existingLayout->addWidget(existingHelp);
        auto* existingForm = new QFormLayout;
        existingForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        base_->setObjectName(QStringLiteral("worktreeBaseWorktree"));
        makeSearchable(base_);
        populateWorktrees(worktrees, repositoryPath);
        existingForm->addRow(tr("Worktree"), base_);
        existingLayout->addLayout(existingForm);
        layout->addWidget(existingBox);

        auto* newBox = new QGroupBox(this);
        newBox->setObjectName(QStringLiteral("newWorktreeSection"));
        auto* newLayout = new QVBoxLayout(newBox);
        newLayout->addWidget(newWorktree_);
        auto* newHelp = new QLabel(
            tr("A new branch is cut from the branch you pick and checked out in a folder next to "
               "%1. The conversation runs there, where it cannot disturb your other work.")
                .arg(QFileInfo(primaryWorktreePath_).fileName()),
            newBox);
        newHelp->setWordWrap(true);
        newHelp->setEnabled(false);
        newLayout->addWidget(newHelp);
        auto* newForm = new QFormLayout;
        newForm->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        branch_->setPlaceholderText(tr("new branch name"));
        newForm->addRow(tr("New branch"), branch_);
        newWorktreeBase_ = new ket::Select(this);
        newWorktreeBase_->setObjectName(QStringLiteral("worktreeBaseBranch"));
        newWorktreeBase_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        newWorktreeBase_->setMinimumContentsLength(40);
        newWorktreeBase_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        makeSearchable(newWorktreeBase_);
        newWorktreeBase_->addItem(tr("Loading branches…"));
        newWorktreeBase_->setEnabled(false);
        newForm->addRow(tr("Branch from"), newWorktreeBase_);
        newLayout->addLayout(newForm);
        status_->setText(tr("Local and remote-tracking branches; no automatic fetch."));
        newLayout->addWidget(status_);
        layout->addWidget(newBox);

        layout->addWidget(error_);
        layout->addWidget(buttons);
        connect(section, &QButtonGroup::buttonToggled, this, [this] { updateSections(); });
        connect(base_, &QComboBox::currentIndexChanged, this, [this] {
            error_->hide();
            // Branches are shared by every checkout, but the branch of the selected
            // worktree is the natural base for one cut from it.
            loadLocalBranches(selectedWorktreePath());
            updateActions();
        });
        updateSections();
    } else {
        auto* layout = new QFormLayout(this);
        layout->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
        base_->setObjectName(QStringLiteral("worktreeBaseBranch"));
        makeSearchable(base_);
        base_->addItem(tr("Loading branches…"));
        base_->setEnabled(false);
        status_->setText(tr("Local and remote-tracking branches; no automatic fetch."));
        layout->addRow(tr("Base branch:"), base_);
        layout->addRow(tr("Branch name:"), branch_);
        layout->addRow(status_);
        layout->addRow(error_);
        layout->addRow(buttons);
    }
    connect(buttons, &QDialogButtonBox::accepted, this, &AgentWorktreeDialog::tryAccept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(branch_, &QLineEdit::textChanged, this, [this] {
        error_->hide();
        updateActions();
    });
    connect(base_, &QComboBox::currentTextChanged, this, [this] { error_->hide(); });

    loadLocalBranches(conversation ? selectedWorktreePath() : repositoryPath);
    updateActions();
    if (conversation)
        base_->setFocus();
    else
        branch_->setFocus();
}

QComboBox* AgentWorktreeDialog::branchCombo() const {
    return purpose_ == Purpose::Conversation ? newWorktreeBase_ : base_;
}

void AgentWorktreeDialog::makeSearchable(QComboBox* combo) {
    // A repository can hold hundreds of refs, so both lists are typed into rather
    // than scrolled: any part of a name matches, in any case.
    combo->setEditable(true);
    combo->setInsertPolicy(QComboBox::NoInsert);
    if (auto* completer = combo->completer()) {
        completer->setFilterMode(Qt::MatchContains);
        completer->setCaseSensitivity(Qt::CaseInsensitive);
        completer->setCompletionMode(QCompleter::PopupCompletion);
    }
}

bool AgentWorktreeDialog::creatingWorktree() const {
    return newWorktree_ != nullptr && newWorktree_->isChecked();
}

// Only the chosen section is live, so the dialog always reads as one decision.
void AgentWorktreeDialog::updateSections() {
    if (purpose_ != Purpose::Conversation)
        return;
    error_->hide();
    const bool creating = creatingWorktree();
    base_->setEnabled(!creating && base_->count() > 0);
    branch_->setEnabled(creating);
    newWorktreeBase_->setEnabled(creating && branchesReady());
    status_->setEnabled(creating);
    updateActions();
}

void AgentWorktreeDialog::setWorktrees(const QVector<GitWorktree>& worktrees) {
    if (purpose_ != Purpose::Conversation)
        return;
    const QString picked = base_->currentData().toString();
    const QSignalBlocker blocker(base_);
    populateWorktrees(worktrees, anchorPath_);
    const int index = picked.isEmpty() ? -1 : base_->findData(picked);
    if (index >= 0)
        base_->setCurrentIndex(index);
    // The signal is blocked while the list is rebuilt, so a selection that moved
    // loads its branches here.
    if (selectedWorktreePath() != picked)
        loadLocalBranches(selectedWorktreePath());
    base_->setEnabled(!creatingWorktree() && base_->count() > 0);
    updateActions();
}

void AgentWorktreeDialog::populateWorktrees(const QVector<GitWorktree>& worktrees,
                                            const QString& repositoryPath) {
    anchorPath_ = repositoryPath;
    base_->clear();
    const QString active = canonicalPath(repositoryPath);
    // The worktree the conversation is being started from wins; the one Git calls
    // current is only a fallback, because the two are compared in different path
    // forms and disagree when a checkout sits behind a symlink.
    int selected = -1;
    int current = -1;
    for (const GitWorktree& worktree : worktrees) {
        if (worktree.bare || worktree.prunable || worktree.path.isEmpty() ||
            !QFileInfo(worktree.path).isDir())
            continue;
        const QString path = canonicalPath(worktree.path);
        base_->addItem(worktreeLabel(worktree), path);
        base_->setItemData(base_->count() - 1, worktree.path, Qt::ToolTipRole);
        if (selected < 0 && path == active)
            selected = base_->count() - 1;
        if (current < 0 && worktree.current)
            current = base_->count() - 1;
    }
    if (selected < 0)
        selected = current;
    if (base_->count() == 0 && !repositoryPath.isEmpty()) {
        GitWorktree fallback;
        fallback.path = repositoryPath;
        base_->addItem(worktreeLabel(fallback), canonicalPath(repositoryPath));
        selected = 0;
    }
    if (selected < 0 && base_->count() > 0)
        selected = 0;
    if (selected >= 0)
        base_->setCurrentIndex(selected);
    base_->setEnabled(base_->count() > 0);
}

void AgentWorktreeDialog::loadLocalBranches(const QString& root) {
    const quint64 generation = ++branchLoadGeneration_;
    branchesReady_ = false;
    setProperty("localBranchesReady", false);
    auto* branches = branchCombo();
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        localBranches_.clear();
        branches->clear();
        status_->setText(tr("Could not load branches. Check the repository and try again."));
        branchesReady_ = true;
        setProperty("localBranchesReady", true);
        updateActions();
        return;
    }
    auto* watcher = new QFutureWatcher<BranchList>(this);
    connect(watcher, &QFutureWatcher<BranchList>::finished, this, [this, watcher, generation] {
        const auto result = watcher->result();
        watcher->deleteLater();
        if (generation != branchLoadGeneration_)
            return;
        auto* branches = branchCombo();
        localBranches_.clear();
        branches->clear();
        if (!result.success) {
            status_->setText(tr("Could not load branches. Check the repository and try again."));
            branchesReady_ = true;
            setProperty("localBranchesReady", true);
            updateActions();
            return;
        }
        const bool conversation = purpose_ == Purpose::Conversation;
        int selected = -1;
        if (!conversation) {
            // Creating a worktree outside a conversation may start from a commit.
            branches->addItem(tr("Current HEAD (detached or current commit)"),
                              QStringLiteral("HEAD"));
            selected = 0;
        }
        for (const auto& line : result.output.split('\n')) {
            const auto fields = line.split('\t');
            if (fields.size() != 3 || !fields[2].isEmpty())
                continue;
            const auto ref = QString::fromUtf8(fields[0]);
            const bool local = ref.startsWith("refs/heads/");
            if (local)
                localBranches_.insert(ref.mid(11));
            // Branch names as Git writes them: `develop`, `origin/develop`.
            branches->addItem(ref.mid(local ? 11 : 13), ref);
            if (fields[1] == "*")
                selected = branches->count() - 1;
        }
        branches->setCurrentIndex(selected < 0 && branches->count() > 0 ? 0 : selected);
        branchesReady_ = true;
        setProperty("localBranchesReady", true);
        if (conversation)
            updateSections();
        else
            branches->setEnabled(true);
        updateActions();
    });
    watcher->setFuture(QtConcurrent::run(loadBranches, root));
}

bool AgentWorktreeDialog::branchesReady() const { return branchesReady_; }

void AgentWorktreeDialog::updateActions() {
    auto* buttons = findChild<QDialogButtonBox*>(QStringLiteral("worktreeDialogButtons"));
    if (buttons == nullptr)
        return;
    auto* ok = buttons->button(QDialogButtonBox::Ok);
    if (purpose_ == Purpose::Conversation) {
        ok->setText(creatingWorktree() ? tr("Create") : tr("Start"));
        ok->setEnabled(creatingWorktree()
                           ? branchesReady_ && !branchName().isEmpty() && !baseBranch().isEmpty()
                           : !selectedWorktreePath().isEmpty());
        return;
    }
    const bool branchesReady = base_->findData(QStringLiteral("HEAD")) >= 0;
    base_->setEnabled(branchesReady);
    ok->setEnabled(branchesReady && !branchName().isEmpty());
}

void AgentWorktreeDialog::tryAccept() {
    error_->hide();
    if (!createWorktree()) {
        if (selectedWorktreePath().isEmpty()) {
            error_->setText(tr("Select a worktree."));
            error_->show();
            return;
        }
        accept();
        return;
    }
    const QString name = branchName();
    if (name.isEmpty()) {
        error_->setText(tr("Enter a branch name."));
        error_->show();
        return;
    }
    if (localBranches_.contains(name)) {
        error_->setText(
            tr("A branch named \"%1\" already exists. Enter a different name and create again.")
                .arg(name));
        error_->show();
        return;
    }
    const QString path = AgentWorktreeCreator::worktreePath(primaryWorktreePath_, name);
    if (QFileInfo::exists(path)) {
        error_->setText(
            tr("A worktree for \"%1\" already exists. Enter a different name and create again.")
                .arg(name));
        error_->show();
        return;
    }
    if (baseBranch().trimmed().isEmpty()) {
        error_->setText(tr("Select a branch to start from."));
        error_->show();
        return;
    }
    accept();
}

QString AgentWorktreeDialog::branchName() const { return branch_->text().trimmed(); }

QString AgentWorktreeDialog::baseBranch() const {
    auto* branches = branchCombo();
    // The combo box is editable, so the typed text can differ from the selected item: typing
    // a name without picking from the completer leaves the old index selected.
    const QString text = branches->currentText().trimmed();
    const int selected = branches->currentIndex();
    if (selected >= 0 && branches->itemText(selected) == text)
        return branches->itemData(selected).toString();
    if (text.isEmpty())
        return {};
    for (int i = 0; i < branches->count(); ++i) {
        if (branches->itemText(i).compare(text, Qt::CaseInsensitive) == 0)
            return branches->itemData(i).toString();
    }
    // A bare name: prefer the local branch, then a remote-tracking one.
    for (const QString& prefix : {QStringLiteral("refs/heads/"), QStringLiteral("refs/remotes/")}) {
        for (int i = 0; i < branches->count(); ++i) {
            const QString ref = branches->itemData(i).toString();
            if (ref.startsWith(prefix) &&
                (ref.mid(prefix.size()) == text || ref.endsWith(QLatin1Char('/') + text)))
                return ref;
        }
    }
    return text;
}

QString AgentWorktreeDialog::selectedWorktreePath() const {
    if (purpose_ != Purpose::Conversation)
        return primaryWorktreePath_;
    // The field is typed into, so the text can name a worktree the index has not
    // moved to yet; the typed name wins when it matches one exactly.
    const QString text = base_->currentText().trimmed();
    const int selected = base_->currentIndex();
    if (selected >= 0 && base_->itemText(selected) == text)
        return base_->itemData(selected).toString();
    for (int i = 0; i < base_->count(); ++i) {
        if (base_->itemText(i).compare(text, Qt::CaseInsensitive) == 0)
            return base_->itemData(i).toString();
    }
    return selected >= 0 ? base_->itemData(selected).toString() : QString{};
}

bool AgentWorktreeDialog::createWorktree() const {
    if (purpose_ != Purpose::Conversation)
        return true;
    return creatingWorktree();
}

AgentConversationDeleteDialog::AgentConversationDeleteDialog(const QStringList& linkedWorktreePaths,
                                                             QWidget* parent)
    : QDialog(parent),
      deleteLinked_(new QCheckBox(static_cast<int>(linkedWorktreePaths.size()) > 1
                                      ? tr("Do you want to delete the %n worktrees linked to this "
                                           "conversation?",
                                           "", static_cast<int>(linkedWorktreePaths.size()))
                                      : tr("Do you want to delete worktrees linked to this "
                                           "conversation?"),
                                  this)),
      force_(
          new QCheckBox(tr("Force delete, discarding uncommitted and untracked changes"), this)) {
    const int linkedWorktreeCount = static_cast<int>(linkedWorktreePaths.size());
    setObjectName(QStringLiteral("conversationDeleteDialog"));
    setWindowTitle(tr("Delete conversation"));
    setMinimumWidth(460);
    auto* layout = new QVBoxLayout(this);
    auto* message = new QLabel(
        tr("Permanently remove KetPlus' local transcript?\n\nProvider history is not deleted, "
           "and any encrypted relay suffix remains until its retention expires."),
        this);
    message->setWordWrap(true);
    layout->addWidget(message);
    deleteLinked_->setObjectName(QStringLiteral("deleteLinkedWorktreeCheck"));
    deleteLinked_->setEnabled(linkedWorktreeCount > 0);
    if (linkedWorktreeCount <= 0)
        deleteLinked_->setToolTip(tr("This conversation is not linked to a dedicated worktree."));
    force_->setObjectName(QStringLiteral("forceDeleteWorktreeCheck"));
    force_->setEnabled(false);
    force_->setToolTip(tr("Removes the worktree even with uncommitted or untracked changes, which "
                          "are lost. Required when Git refuses a dirty, untracked, or locked "
                          "worktree."));
    layout->addWidget(deleteLinked_);
    if (linkedWorktreeCount > 0) {
        auto* paths = new QLabel(QStringList(linkedWorktreePaths).join(QLatin1Char('\n')), this);
        paths->setObjectName(QStringLiteral("linkedWorktreePaths"));
        paths->setTextInteractionFlags(Qt::TextSelectableByMouse);
        paths->setWordWrap(true);
        paths->setContentsMargins(22, 0, 0, 0);
        layout->addWidget(paths);
    }
    layout->addWidget(force_);
    // The consequence stays visible next to the choice instead of only in a tooltip.
    auto* forceWarning =
        new QLabel(tr("Uncommitted and untracked files in these worktrees are permanently "
                      "deleted. Commit or stash anything you want to keep first."),
                   this);
    forceWarning->setObjectName(QStringLiteral("forceDeleteWorktreeWarning"));
    forceWarning->setWordWrap(true);
    forceWarning->setContentsMargins(22, 0, 0, 0);
    forceWarning->setVisible(false);
    layout->addWidget(forceWarning);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    auto* deleteButton = buttons->button(QDialogButtonBox::Ok);
    deleteButton->setText(tr("Delete"));
    layout->addWidget(buttons);
    connect(deleteLinked_, &QCheckBox::toggled, this, [this](const bool checked) {
        force_->setEnabled(checked);
        if (!checked)
            force_->setChecked(false);
    });
    connect(force_, &QCheckBox::toggled, this, [forceWarning, deleteButton](const bool checked) {
        forceWarning->setVisible(checked);
        deleteButton->setText(checked ? tr("Delete and Discard Changes") : tr("Delete"));
    });
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
}

bool AgentConversationDeleteDialog::deleteLinkedWorktree() const {
    return deleteLinked_->isChecked();
}

bool AgentConversationDeleteDialog::forceDeleteWorktree() const { return force_->isChecked(); }

} // namespace ketplus

#include "teams/TeamTaskDialog.h"

#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetFieldRow.h"
#include "KetIcon.h"
#include "KetMarkdownView.h"
#include "KetSelect.h"
#include "KetTextField.h"
#include "KetTokens.h"
#include "teams/TeamDirectory.h"
#include "teams/TeamMenus.h"
#include "teams/TeamPageParts.h"

#include <QDateTime>
#include <QEvent>
#include <QGridLayout>
#include <QJsonArray>
#include <QLabel>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>

namespace ketplus {
namespace {

using team_page::px;
using team_page::Text;

// A description taller than this is cut to kCutHeight until the user asks for all of it.
constexpr int kCutAbove = 168;
constexpr int kCutHeight = 150;

QColor token(const QStringView name) {
    return ket::Tokens::color(name, ket::Appearance::instance().theme());
}

QJsonObject taskOf(const QJsonObject& team, const QString& taskId) {
    for (const auto& value : team.value(QStringLiteral("tasks")).toArray())
        if (value.toObject().value(QStringLiteral("id")).toString() == taskId)
            return value.toObject();
    return {};
}

ket::BadgeTone stateTone(const QString& state) {
    if (state == QStringLiteral("in_progress"))
        return ket::BadgeTone::Accent;
    if (state == QStringLiteral("in_review"))
        return ket::BadgeTone::Info;
    if (state == QStringLiteral("blocked"))
        return ket::BadgeTone::Warning;
    if (state == QStringLiteral("ready") || state == QStringLiteral("done"))
        return ket::BadgeTone::Success;
    return ket::BadgeTone::Neutral;
}

Text* heading(const QString& value, QWidget* parent, const char* name) {
    auto* label = new Text(value, parent, Text::Fit::Elide);
    label->setObjectName(QLatin1String(name));
    label->setType(14, QFont::Medium);
    return label;
}

class Rule final : public QWidget {
  public:
    explicit Rule(QWidget* parent) : QWidget(parent) { setFixedHeight(1); }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter(this).fillRect(rect(), token(u"panel-border"));
    }
};

// Fades the cut description into the dialog.
class Fade final : public QWidget {
  public:
    explicit Fade(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        const QColor solid = ket::dialogRecipe(ket::Appearance::instance().theme()).background;
        QColor clear = solid;
        clear.setAlpha(0);
        QLinearGradient gradient(0, 0, 0, height());
        gradient.setColorAt(0, clear);
        gradient.setColorAt(1, solid);
        QPainter(this).fillRect(rect(), gradient);
    }
};

// The description at its cut or its whole height, with the fade over the cut.
class Clip final : public QWidget {
  public:
    Clip(ket::MarkdownView* view, const bool whole, QWidget* parent)
        : QWidget(parent), view_(view), fade_(new Fade(this)), whole_(whole) {
        view->setParent(this);
        fade_->raise();
        QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        policy.setHeightForWidth(true);
        setSizePolicy(policy);
    }

    [[nodiscard]] bool cut(const int width) const {
        return view_->heightForWidth(width) > px(kCutAbove);
    }
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(const int width) const override {
        return cut(width) && !whole_ ? px(kCutHeight) : view_->heightForWidth(width);
    }
    [[nodiscard]] QSize sizeHint() const override {
        return {view_->sizeHint().width(), heightForWidth(qMax(width(), px(200)))};
    }

  protected:
    void resizeEvent(QResizeEvent*) override {
        view_->setGeometry(0, 0, width(), view_->heightForWidth(width()));
        const bool shown = cut(width()) && !whole_;
        fade_->setVisible(shown);
        fade_->setGeometry(0, height() - px(48), width(), px(48));
    }

  private:
    ket::MarkdownView* view_;
    Fade* fade_;
    bool whole_;
};

// Scrolls the task. It asks for the task's whole height; the dialog gives it what the window
// leaves, and the rest scrolls while the dialog's actions stay in view.
class Body final : public QScrollArea {
  public:
    using QScrollArea::QScrollArea;

    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(const int width) const override {
        const QWidget* inner = widget();
        if (!inner || !inner->layout())
            return px(120);
        return inner->layout()->hasHeightForWidth() ? inner->layout()->totalHeightForWidth(width)
                                                    : inner->sizeHint().height();
    }
    [[nodiscard]] QSize sizeHint() const override {
        return {px(400), heightForWidth(qMax(width(), px(200)))};
    }
    [[nodiscard]] QSize minimumSizeHint() const override { return {px(160), px(120)}; }
};

} // namespace

TeamTaskDialog::TeamTaskDialog(TeamDirectory* teams, const QString& teamId, const QString& taskId,
                               QWidget* parent)
    : QWidget(parent), teams_(teams), teamId_(teamId), taskId_(taskId), scroll_(new Body(this)),
      form_(new QWidget(this)), owner_(new ket::Select(form_)), reason_(new ket::TextField(form_)),
      error_(new Text({}, form_)) {
    setObjectName(QStringLiteral("teamTaskDialogBody"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    scroll_->setObjectName(QStringLiteral("teamTaskScroll"));
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->viewport()->setAutoFillBackground(false);
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    scroll_->setSizePolicy(policy);
    layout->addWidget(scroll_);

    // The handover's fields; openHandover() shows them in a dialog of their own.
    form_->hide();
    auto* fields = new QVBoxLayout(form_);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setSpacing(px(14));
    owner_->setObjectName(QStringLiteral("teamHandoverOwner"));
    reason_->setObjectName(QStringLiteral("teamHandoverReason"));
    reason_->setPlaceholderText(tr("Why it changes hands"));
    fields->addWidget(new ket::FieldRow(
        tr("New owner"), owner_,
        tr("Gets the task's history, pull request, branch, worktree and the decisions made "
           "for it."),
        form_));
    fields->addWidget(new ket::FieldRow(tr("Reason"), reason_, {}, form_));
    error_->setObjectName(QStringLiteral("teamTaskError"));
    static_cast<Text*>(error_)->setTone(QStringLiteral("danger"));
    error_->setVisible(false);
    fields->addWidget(error_);

    connect(teams_, &TeamDirectory::changed, this, [this] {
        rebuild();
        readHistory();
    });
    connect(&ket::Appearance::instance(), &ket::Appearance::themeChanged, this,
            &TeamTaskDialog::rebuild);
    connect(&ket::Appearance::instance(), &ket::Appearance::zoomChanged, this,
            &TeamTaskDialog::rebuild);
    rebuild();
    readHistory();
}

QString TeamTaskDialog::error() const { return error_->isHidden() ? QString{} : error_->text(); }

void TeamTaskDialog::rebuild() {
    const int position = scroll_->verticalScrollBar()->value();
    details_ = new QWidget;
    details_->setObjectName(QStringLiteral("teamTaskDetails"));
    auto* column = new QVBoxLayout(details_);
    // The scroll bar keeps clear of the text.
    column->setContentsMargins(0, 0, px(14), 0);
    column->setSpacing(px(14));
    const auto team = teams_->team(teamId_);
    const QJsonObject task = team ? taskOf(*team, taskId_) : QJsonObject{};
    ownerSession_.clear();
    canHandOver_ = false;
    if (task.isEmpty()) {
        auto* gone = new Text(tr("This task no longer exists."), details_);
        gone->setObjectName(QStringLiteral("teamTaskGone"));
        gone->setTone(QStringLiteral("text-secondary"));
        column->addWidget(gone);
        owner_->setEnabled(false);
        scroll_->setWidget(details_);
        details_->setAutoFillBackground(false);
        fitHeight();
        emit actionsChanged(false, false);
        return;
    }
    const QString state = task.value(QStringLiteral("state")).toString();
    ownerSession_ = task.value(QStringLiteral("owner")).toString();

    // State and lane, and the flag of an edit outside the lane.
    auto* badges = new QWidget(details_);
    auto* flow = new team_page::Flow(badges, px(8), px(6));
    auto* stateBadge = new ket::Badge(team_states::label(state), stateTone(state), badges);
    stateBadge->setObjectName(QStringLiteral("teamTaskState"));
    flow->addWidget(stateBadge);
    const QString lane = task.value(QStringLiteral("lane")).toString();
    flow->addWidget(
        new ket::Badge(lane.isEmpty() ? tr("Whole epic") : lane, ket::BadgeTone::Neutral, badges));
    if (!task.value(QStringLiteral("outside_lane")).toArray().isEmpty())
        flow->addWidget(new ket::Badge(tr("Outside its lane"), ket::BadgeTone::Warning, badges));
    column->addWidget(badges);

    // The facts, two to a line: a muted label over its value.
    const QString none = QStringLiteral("–");
    QStringList depends;
    for (const auto& value : task.value(QStringLiteral("depends_on")).toArray())
        depends.append(value.toString());
    QString reviewer;
    for (const auto& value : team->value(QStringLiteral("members")).toArray())
        if (value.toObject().value(QStringLiteral("role")).toString() == QStringLiteral("reviewer"))
            reviewer = value.toObject().value(QStringLiteral("name")).toString();
    const QJsonObject ownerMember = TeamDirectory::member(*team, ownerSession_);
    const QString ownerName = ownerSession_.isEmpty() ? tr("Unassigned")
                              : ownerMember.isEmpty()
                                  ? ownerSession_
                                  : ownerMember.value(QStringLiteral("name")).toString();
    struct Fact {
        QString label;
        QString value;
        const char* name;
        bool mono;
    };
    const QList<Fact> facts{
        {tr("Owner"), ownerName, "teamTaskOwner", false},
        {tr("Reviewer"), reviewer, "teamTaskReviewer", false},
        {tr("Pull request"), task.value(QStringLiteral("pull_request")).toString(),
         "teamTaskPullRequest", true},
        {tr("Branch"), task.value(QStringLiteral("branch")).toString(), "teamTaskBranch", true},
        {tr("Worktree"), task.value(QStringLiteral("worktree")).toString(), "teamTaskWorktree",
         true},
        {tr("Depends on"), depends.join(QStringLiteral(", ")), "teamTaskDependencies", true}};
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(px(16));
    grid->setVerticalSpacing(px(14));
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    for (int index = 0; index < facts.size(); ++index) {
        const Fact& fact = facts.at(index);
        auto* label = new Text(fact.label, details_, Text::Fit::Elide);
        label->setTone(QStringLiteral("text-muted"));
        label->setType(12);
        const bool has = !fact.value.isEmpty();
        // A path or a branch is cut in the middle, where it says least.
        auto* value = new Text(has ? fact.value : none, details_, Text::Fit::ElideMiddle);
        value->setObjectName(QLatin1String(fact.name));
        value->setType(has && fact.mono ? 12 : 13, has && fact.mono ? QFont::Medium : QFont::Normal,
                       has && fact.mono);
        value->setAccessibleName(QStringLiteral("%1: %2").arg(fact.label, has ? fact.value : none));
        auto* cell = new QVBoxLayout;
        cell->setSpacing(px(4));
        cell->addWidget(label);
        cell->addWidget(value);
        grid->addLayout(cell, index / 2, index % 2);
    }
    column->addLayout(grid);

    // What the leader wrote for the owner, as Markdown.
    if (const QString description = task.value(QStringLiteral("description")).toString();
        !description.trimmed().isEmpty()) {
        column->addWidget(new Rule(details_));
        column->addWidget(heading(tr("Description"), details_, "teamTaskDescriptionHeading"));
        auto* markdown = new ket::MarkdownView(description);
        markdown->setObjectName(QStringLiteral("teamTaskDescription"));
        auto* clip = new Clip(markdown, wholeDescription_, details_);
        clip->setObjectName(QStringLiteral("teamTaskDescriptionClip"));
        column->addWidget(clip);
        auto* all = new ket::Button(wholeDescription_ ? tr("Show less") : tr("Show all"),
                                    ket::ButtonVariant::Ghost, details_);
        all->setObjectName(QStringLiteral("teamTaskDescriptionAll"));
        all->setControlSize(ket::ControlSize::Small);
        // Only a description that is cut has more to show; the body's width decides.
        all->setProperty("clip", QVariant::fromValue<QWidget*>(clip));
        connect(all, &QAbstractButton::clicked, this, [this] {
            wholeDescription_ = !wholeDescription_;
            rebuild();
        });
        column->addWidget(all, 0, Qt::AlignLeft);
    }

    column->addWidget(new Rule(details_));
    const QJsonArray criteria = task.value(QStringLiteral("criteria")).toArray();
    const auto met = std::count_if(criteria.begin(), criteria.end(), [](const auto& value) {
        return value.toObject().value(QStringLiteral("met")).toBool();
    });
    auto* criteriaTitle = new QHBoxLayout;
    criteriaTitle->setSpacing(px(8));
    criteriaTitle->addWidget(
        heading(tr("Acceptance criteria"), details_, "teamTaskCriteriaHeading"));
    auto* count = new Text(criteria.isEmpty() ? tr("None yet")
                                              : tr("%1 of %2 met").arg(met).arg(criteria.size()),
                           details_, Text::Fit::Elide);
    count->setObjectName(QStringLiteral("teamTaskCriteriaCount"));
    count->setTone(QStringLiteral("text-secondary"));
    criteriaTitle->addWidget(count, 1);
    column->addLayout(criteriaTitle);
    for (const auto& value : criteria) {
        const QJsonObject criterion = value.toObject();
        const bool done = criterion.value(QStringLiteral("met")).toBool();
        auto* line = new QHBoxLayout;
        line->setSpacing(px(10));
        // Not colour alone: the icon differs and says so to a screen reader.
        auto* mark =
            new ket::Icon(done ? QStringLiteral("check") : QStringLiteral("circle"), 14, details_);
        mark->setColor(token(done ? u"success" : u"text-muted"));
        mark->setAccessibleName(done ? tr("Met") : tr("Not met yet"));
        line->addWidget(mark, 0, Qt::AlignTop);
        auto* text = new Text(criterion.value(QStringLiteral("text")).toString(), details_);
        text->setObjectName(QStringLiteral("teamTaskCriterion"));
        line->addWidget(text, 1);
        column->addLayout(line);
    }
    auto* note = new QHBoxLayout;
    note->setSpacing(px(8));
    auto* lock = new ket::Icon(QStringLiteral("lock"), 13, details_);
    lock->setColor(token(u"text-muted"));
    note->addWidget(lock, 0, Qt::AlignTop);
    auto* rule = new Text(tr("Only the leader changes these. The reviewer checks the pull "
                             "request against them."),
                          details_);
    rule->setTone(QStringLiteral("text-muted"));
    rule->setType(12);
    note->addWidget(rule, 1);
    column->addLayout(note);

    column->addWidget(new Rule(details_));
    column->addWidget(heading(tr("History"), details_, "teamTaskHistoryHeading"));
    // ADR 0115: an older daemon still sends the whole history with the team.
    if (historyRead_ && historyStart_ > 0) {
        auto* earlier = new ket::Button(tr("Show earlier (%1)").arg(historyStart_),
                                        ket::ButtonVariant::Ghost, details_);
        earlier->setObjectName(QStringLiteral("teamTaskHistoryEarlier"));
        earlier->setControlSize(ket::ControlSize::Small);
        connect(earlier, &QAbstractButton::clicked, this, [this] { readHistory(true); });
        column->addWidget(earlier, 0, Qt::AlignLeft);
    }
    const QJsonArray history =
        historyRead_ ? history_ : task.value(QStringLiteral("history")).toArray();
    for (const auto& value : history) {
        const QJsonObject entry = value.toObject();
        const QString actor = entry.value(QStringLiteral("actor")).toString();
        const QJsonObject who = TeamDirectory::member(*team, actor);
        const QDateTime at =
            QDateTime::fromString(entry.value(QStringLiteral("at")).toString(), Qt::ISODateWithMs)
                .toLocalTime();
        auto* line = new QHBoxLayout;
        line->setSpacing(px(12));
        auto* time = new Text(at.toString(at.date() == QDate::currentDate()
                                              ? QStringLiteral("HH:mm")
                                              : QStringLiteral("d MMM, HH:mm")),
                              details_, Text::Fit::Elide);
        time->setTone(QStringLiteral("text-muted"));
        time->setType(12);
        time->setFixedWidth(px(96));
        line->addWidget(time, 0, Qt::AlignTop);
        auto* text = new Text(QStringLiteral("%1 · %2").arg(
                                  actor == QStringLiteral("user")     ? tr("You")
                                  : actor == QStringLiteral("daemon") ? tr("ketPlus")
                                  : who.isEmpty() ? actor
                                                  : who.value(QStringLiteral("name")).toString(),
                                  entry.value(QStringLiteral("text")).toString()),
                              details_);
        text->setObjectName(QStringLiteral("teamTaskHistory"));
        text->setTone(QStringLiteral("text-secondary"));
        line->addWidget(text, 1);
        column->addLayout(line);
    }
    column->addStretch();
    scroll_->setWidget(details_); // deletes the details before
    details_->setAutoFillBackground(false);
    fitHeight();
    scroll_->verticalScrollBar()->setValue(position);

    // Another developer of the team; keep the choice across updates.
    const QString chosen = owner_->currentData().toString();
    const QSignalBlocker block(owner_);
    owner_->clear();
    for (const auto& value : team->value(QStringLiteral("members")).toArray()) {
        const QJsonObject candidate = value.toObject();
        if (candidate.value(QStringLiteral("role")).toString() == QStringLiteral("developer") &&
            candidate.value(QStringLiteral("session_id")).toString() != ownerSession_)
            owner_->addItem(candidate.value(QStringLiteral("name")).toString(),
                            candidate.value(QStringLiteral("session_id")).toString());
    }
    const int index = owner_->findData(chosen);
    owner_->setCurrentIndex(index >= 0 ? index : 0);
    canHandOver_ = owner_->count() > 0 && state != QStringLiteral("done");
    owner_->setEnabled(canHandOver_);
    emit actionsChanged(!ownerSession_.isEmpty(), canHandOver_);
}

// The task changed height: the dialog sizes its card again, up to what the window leaves.
void TeamTaskDialog::fitHeight() {
    if (!details_)
        return;
    scroll_->updateGeometry();
    // Show all only where the description is cut at this width.
    const int width = qMax(px(200), scroll_->viewport()->width());
    if (auto* all = details_->findChild<ket::Button*>(QStringLiteral("teamTaskDescriptionAll")))
        if (auto* clip = static_cast<Clip*>(all->property("clip").value<QWidget*>()))
            all->setVisible(wholeDescription_ || clip->cut(width - px(14)));
    for (QWidget* up = parentWidget(); up; up = up->parentWidget())
        if (auto* dialog = qobject_cast<ket::Dialog*>(up)) {
            if (dialog->isVisible())
                dialog->setCardWidth(kCardWidth);
            break;
        }
}

void TeamTaskDialog::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    // The width decides whether the description is cut.
    if (event->size().width() != event->oldSize().width())
        fitHeight();
}

bool TeamTaskDialog::event(QEvent* event) { return QWidget::event(event); }

} // namespace ketplus

#include "schedules/ScheduleWindow.h"

#include "schedules/ScheduleClient.h"
#include "schedules/ScheduleEditor.h"
#include "schedules/ScheduleRules.h"

#include "KetAppearance.h"
#include "KetBadge.h"
#include "KetButton.h"
#include "KetDialog.h"
#include "KetEmptyState.h"
#include "KetSelect.h"
#include "KetTextArea.h"
#include "KetTextField.h"
#include "KetToggle.h"

#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPointer>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QTextEdit>
#include <QVBoxLayout>

namespace ketplus {
namespace {

int scaled(const qreal logical) { return qRound(ket::Appearance::instance().scaled(logical)); }

QLabel* fact(QFormLayout* form, const QString& label) {
    auto* value = new QLabel;
    value->setWordWrap(true);
    value->setTextInteractionFlags(Qt::TextSelectableByMouse);
    form->addRow(label, value);
    return value;
}

} // namespace

ScheduleWindow::ScheduleWindow(ScheduleClient* client, QWidget* parent)
    : QWidget(parent), client_(client) {
    setWindowFlag(Qt::Window);
    setWindowTitle(tr("Schedules"));
    setObjectName(QStringLiteral("schedulesWindow"));
    resize(scaled(1160), scaled(800));
    setMinimumSize(scaled(900), scaled(600));
    build();
    connect(client_, &ScheduleClient::changed, this, &ScheduleWindow::reload);
    connect(client_, &ScheduleClient::runsChanged, this, [this](const QString& id) {
        if (id == selectedId_)
            reloadDetail();
    });
    reload();
}

void ScheduleWindow::present(const QString& scheduleId) {
    if (!scheduleId.isEmpty())
        selectedId_ = scheduleId;
    reload();
}

void ScheduleWindow::edit(const QString& scheduleId) {
    present(scheduleId);
    editSelected();
}

void ScheduleWindow::build() {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(scaled(20), scaled(16), scaled(20), scaled(16));
    root->setSpacing(scaled(12));

    auto* header = new QHBoxLayout;
    auto* title = new QLabel(tr("Schedules"), this);
    QFont titleFont = title->font();
    titleFont.setPixelSize(scaled(22));
    titleFont.setWeight(QFont::DemiBold);
    title->setFont(titleFont);
    count_ = new ket::Badge(QStringLiteral("0"), ket::BadgeTone::Neutral, this);
    count_->setObjectName(QStringLiteral("schedulesCount"));
    filter_ = new ket::Select(this);
    filter_->setObjectName(QStringLiteral("schedulesFilter"));
    filter_->addItems({tr("All schedules"), tr("Enabled"), tr("Paused"), tr("Agent-created")});
    filter_->setMinimumWidth(scaled(160));
    search_ = new ket::TextField(this);
    search_->setObjectName(QStringLiteral("schedulesSearch"));
    search_->setPlaceholderText(tr("Search schedules…"));
    search_->setLeadingIcon(QStringLiteral("search"));
    search_->setMinimumWidth(scaled(220));
    header->addWidget(title);
    header->addWidget(count_, 0, Qt::AlignVCenter);
    header->addStretch(1);
    header->addWidget(filter_);
    header->addWidget(search_);
    root->addLayout(header);

    banner_ = new QLabel(this);
    banner_->setObjectName(QStringLiteral("schedulesBanner"));
    banner_->setWordWrap(true);
    banner_->hide();
    root->addWidget(banner_);

    stack_ = new QStackedWidget(this);
    empty_ = new ket::EmptyState(QStringLiteral("clock"), tr("No schedules yet"),
                                 tr("Create schedules from New conversation or New team."), stack_);
    empty_->setObjectName(QStringLiteral("schedulesEmpty"));
    empty_->setAction(tr("Clear filters"));
    empty_->action()->setObjectName(QStringLiteral("schedulesClearFilters"));
    empty_->action()->hide();
    connect(empty_->action(), &QAbstractButton::clicked, this, [this] {
        search_->clear();
        filter_->setCurrentIndex(0);
    });
    stack_->addWidget(empty_);

    list_ = new QListWidget;
    list_->setObjectName(QStringLiteral("schedulesList"));
    list_->setFrameShape(QFrame::NoFrame);
    list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list_->setSpacing(0);
    list_->setMinimumWidth(scaled(320));
    // The row paints its own selection. The item highlight was covering the checkbox frame.
    list_->setStyleSheet(
        QStringLiteral("QListWidget::item { background: transparent; border: none; }"
                       "QListWidget::item:selected { background: transparent; }"));
    connect(list_, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current, QListWidgetItem*) {
                if (rebuilding_ || current == nullptr)
                    return;
                select(current->data(Qt::UserRole).toString());
            });

    detail_ = new QWidget;
    auto* detailColumn = new QVBoxLayout(detail_);
    auto* nameRow = new QHBoxLayout;
    detailName_ = new QLabel(detail_);
    QFont nameFont = detailName_->font();
    nameFont.setPixelSize(scaled(18));
    nameFont.setWeight(QFont::DemiBold);
    detailName_->setFont(nameFont);
    detailName_->setWordWrap(true);
    detailEnabled_ = new ket::Checkbox(tr("Enabled"), detail_);
    detailEnabled_->setObjectName(QStringLiteral("scheduleEnabled"));
    connect(detailEnabled_, &QAbstractButton::toggled, this, [this](bool enabled) {
        if (!rebuilding_)
            applyEnabled(selectedId_, enabled);
    });
    nameRow->addWidget(detailName_, 1);
    nameRow->addWidget(detailEnabled_, 0, Qt::AlignTop);
    detailColumn->addLayout(nameRow);

    auto* form = new QFormLayout;
    target_ = fact(form, tr("Target"));
    repeat_ = fact(form, tr("Repeat"));
    busy_ = fact(form, tr("When busy"));
    missed_ = fact(form, tr("When missed"));
    createdBy_ = fact(form, tr("Created by"));
    nextRun_ = fact(form, tr("Next run"));
    detailColumn->addLayout(form);

    prompt_ = new ket::TextArea(detail_);
    prompt_->setObjectName(QStringLiteral("schedulePrompt"));
    prompt_->editor()->setReadOnly(true);
    prompt_->setMinimumHeight(scaled(96));
    detailColumn->addWidget(prompt_);

    auto* historyTitle = new QLabel(tr("Run history"), detail_);
    detailColumn->addWidget(historyTitle);
    history_ = new QWidget(detail_);
    history_->setObjectName(QStringLiteral("scheduleRunHistory"));
    new QVBoxLayout(history_);
    detailColumn->addWidget(history_);
    showMore_ = new ket::Button(tr("Show more"), ket::ButtonVariant::Ghost, detail_);
    showMore_->setObjectName(QStringLiteral("scheduleShowMore"));
    connect(showMore_, &QAbstractButton::clicked, this,
            [this] { client_->loadRuns(selectedId_, true); });
    detailColumn->addWidget(showMore_, 0, Qt::AlignLeft);
    detailColumn->addStretch(1);

    auto* actions = new QHBoxLayout;
    runNowButton_ = new ket::Button(tr("Run now"), ket::ButtonVariant::Primary, detail_);
    runNowButton_->setObjectName(QStringLiteral("scheduleRunNow"));
    pause_ = new ket::Button(tr("Pause"), ket::ButtonVariant::Secondary, detail_);
    pause_->setObjectName(QStringLiteral("schedulePause"));
    edit_ = new ket::Button(tr("Edit"), ket::ButtonVariant::Secondary, detail_);
    edit_->setObjectName(QStringLiteral("scheduleEdit"));
    remove_ = new ket::Button(tr("Delete"), ket::ButtonVariant::Danger, detail_);
    remove_->setObjectName(QStringLiteral("scheduleDelete"));
    connect(runNowButton_, &QAbstractButton::clicked, this, &ScheduleWindow::runNow);
    connect(pause_, &QAbstractButton::clicked, this, [this] {
        const auto schedule = selectedSchedule();
        if (!schedule.isEmpty())
            applyEnabled(selectedId_, !schedule.value(QStringLiteral("enabled")).toBool());
    });
    connect(edit_, &QAbstractButton::clicked, this, &ScheduleWindow::editSelected);
    connect(remove_, &QAbstractButton::clicked, this, &ScheduleWindow::confirmDelete);
    actions->addWidget(runNowButton_);
    actions->addWidget(pause_);
    actions->addWidget(edit_);
    actions->addWidget(remove_);
    actions->addStretch(1);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(detail_);
    auto* pane = new QWidget;
    auto* paneLayout = new QVBoxLayout(pane);
    paneLayout->setContentsMargins(scaled(16), scaled(12), scaled(16), scaled(12));
    paneLayout->addWidget(scroll, 1);
    paneLayout->addLayout(actions);
    auto* splitter = new QSplitter;
    splitter->addWidget(list_);
    splitter->addWidget(pane);
    splitter->setStretchFactor(1, 1);
    stack_->addWidget(splitter);
    root->addWidget(stack_, 1);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { reload(); });
    connect(filter_, &QComboBox::currentIndexChanged, this, [this](int) { reload(); });
}

void ScheduleWindow::select(const QString& id) {
    if (selectedId_ == id)
        return;
    selectedId_ = id;
    actionError_.clear();
    refreshBanner();
    reloadDetail();
    if (!id.isEmpty() && !client_->runsLoaded(id))
        client_->loadRuns(id, false);
}

void ScheduleWindow::applyEnabled(const QString& id, const bool enabled) {
    if (id.isEmpty())
        return;
    actionError_.clear();
    refreshBanner();
    const QPointer<ScheduleWindow> alive(this);
    client_->request(QStringLiteral("schedule.setEnabled"),
                     {{QStringLiteral("id"), id}, {QStringLiteral("enabled"), enabled}},
                     [alive](const QJsonObject& result, const QString& error) {
                         if (alive && !error.isEmpty())
                             alive->showFailure(result, error);
                     });
}

void ScheduleWindow::runNow() {
    if (selectedId_.isEmpty())
        return;
    actionError_.clear();
    refreshBanner();
    const QPointer<ScheduleWindow> alive(this);
    client_->request(QStringLiteral("schedule.runNow"), {{QStringLiteral("id"), selectedId_}},
                     [alive](const QJsonObject& result, const QString& error) {
                         if (alive && !error.isEmpty())
                             alive->showFailure(result, error);
                     });
}

void ScheduleWindow::editSelected() {
    const QJsonObject schedule = selectedSchedule();
    if (schedule.isEmpty())
        return;
    const QString id = schedule.value(QStringLiteral("id")).toString();
    if (ScheduleEditor* existing = editor_) {
        if (existing->scheduleId() == id) {
            existing->open();
            return;
        }
        delete existing;
    }
    editor_ = new ScheduleEditor(client_, schedule, this);
    editor_->open();
}

void ScheduleWindow::confirmDelete() {
    const QJsonObject schedule = selectedSchedule();
    if (schedule.isEmpty())
        return;
    const QString id = schedule.value(QStringLiteral("id")).toString();
    auto* dialog = new ket::Dialog(tr("Delete schedule?"), this);
    dialog->setObjectName(QStringLiteral("scheduleDeleteDialog"));
    auto* body = new QLabel(tr("Past conversations stay. This only removes the schedule."));
    body->setWordWrap(true);
    dialog->setBody(body);
    auto* cancel = dialog->addButton(tr("Cancel"));
    connect(cancel, &QAbstractButton::clicked, dialog, &ket::Dialog::reject);
    auto* confirm = dialog->addButton(tr("Delete"), ket::ButtonVariant::Danger);
    confirm->setObjectName(QStringLiteral("scheduleDeleteConfirm"));
    const QPointer<ScheduleWindow> alive(this);
    connect(confirm, &QAbstractButton::clicked, this, [alive, dialog, id] {
        dialog->accept();
        if (!alive)
            return;
        alive->client_->request(QStringLiteral("schedule.delete"), {{QStringLiteral("id"), id}},
                                [alive](const QJsonObject& result, const QString& error) {
                                    if (alive && !error.isEmpty())
                                        alive->showFailure(result, error);
                                });
    });
    connect(dialog, &ket::Dialog::finished, dialog, &QObject::deleteLater);
    dialog->open();
}

void ScheduleWindow::showFailure(const QJsonObject& result, const QString& error) {
    actionError_ = scheduleFailureText(result, error);
    refreshBanner();
}

void ScheduleWindow::refreshBanner() {
    const QString text = !actionError_.isEmpty() ? actionError_ : client_->listError();
    banner_->setText(text);
    banner_->setVisible(!text.isEmpty());
}

QJsonObject ScheduleWindow::selectedSchedule() const {
    const auto schedule = client_->schedule(selectedId_);
    return schedule ? schedule::toJson(*schedule) : QJsonObject{};
}

} // namespace ketplus

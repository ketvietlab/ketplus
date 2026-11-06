#include "remote/RemoteActivitySection.h"

#include "DaemonClient.h"
#include "ui/ChromeGeometry.h"
#include "ui/LucideIcon.h"

#include <QAccessible>
#include <QAccessibleWidget>
#include <QApplication>
#include <QDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QToolTip>
#include <QVBoxLayout>

#include <utility>

namespace ketplus {

namespace remote_activity {

QString action(const QString& kind) {
    static const QHash<QString, QString> actions{
        {QStringLiteral("session.start_turn"), QStringLiteral("Sent a message")},
        {QStringLiteral("session.steer"), QStringLiteral("Steered a turn")},
        {QStringLiteral("session.interrupt"), QStringLiteral("Stopped a turn")},
        {QStringLiteral("session.create"), QStringLiteral("Started a conversation")},
        {QStringLiteral("session.delete"), QStringLiteral("Deleted a conversation")},
        {QStringLiteral("session.rename"), QStringLiteral("Renamed a conversation")},
        {QStringLiteral("session.fork"), QStringLiteral("Forked a conversation")},
        {QStringLiteral("session.worktree.switch"), QStringLiteral("Switched worktree")},
        {QStringLiteral("session.configuration.apply"),
         QStringLiteral("Changed conversation settings")},
        {QStringLiteral("session.execution_policy.apply"), QStringLiteral("Changed permissions")},
        {QStringLiteral("session.mode.apply"), QStringLiteral("Changed the mode")},
        {QStringLiteral("session.task.stop"), QStringLiteral("Stopped a background task")},
        {QStringLiteral("approval.resolve"), QStringLiteral("Answered an approval")},
        {QStringLiteral("question.answer"), QStringLiteral("Answered a question")},
        {QStringLiteral("atlas.start"), QStringLiteral("Started Atlas")},
        {QStringLiteral("atlas.detach"), QStringLiteral("Stopped Atlas")}};
    return actions.value(kind, QStringLiteral("Sent a %1 request").arg(kind));
}

Outcome outcome(const QString& resultCode) {
    if (resultCode == QStringLiteral("ok"))
        return {QStringLiteral("Done"), QStringLiteral("positive"), {}};
    if (resultCode == QStringLiteral("worktree_quota_exceeded"))
        return {QStringLiteral("Refused"), QStringLiteral("warning"),
                QStringLiteral("Worktree limit reached")};
    // The device id of such a request is only the one it claimed.
    if (resultCode == QStringLiteral("invalid_encrypted_command") ||
        resultCode == QStringLiteral("sender_key_unavailable"))
        return {QStringLiteral("Refused"), QStringLiteral("danger"),
                QStringLiteral("Sender not verified")};
    return {QStringLiteral("Refused"), QStringLiteral("warning"), resultCode};
}

QString when(const QDateTime& recordedAt, const QDateTime& now) {
    const qint64 seconds = qMax<qint64>(0, recordedAt.secsTo(now));
    if (seconds < 60)
        return QStringLiteral("Just now");
    if (seconds < 3600)
        return QStringLiteral("%1 min ago").arg(seconds / 60);
    const QDateTime local = recordedAt.toLocalTime();
    const QDate today = now.toLocalTime().date();
    const QString time = local.toString(QStringLiteral("HH:mm"));
    if (local.date() == today) {
        const qint64 hours = seconds / 3600;
        return hours == 1 ? QStringLiteral("1 hour ago")
                          : QStringLiteral("%1 hours ago").arg(hours);
    }
    if (local.date() == today.addDays(-1))
        return QStringLiteral("Yesterday, %1").arg(time);
    const QLocale english(QLocale::English);
    if (local.date().daysTo(today) < 7)
        return QStringLiteral("%1, %2").arg(
            english.dayName(local.date().dayOfWeek(), QLocale::ShortFormat), time);
    return english.toString(local, QStringLiteral("d MMM, HH:mm"));
}

QString target(const QJsonObject& entry) {
    if (entry.value(QStringLiteral("session_id")).toString().isEmpty())
        return {};
    if (!entry.value(QStringLiteral("session_exists")).toBool())
        return QStringLiteral("Deleted conversation");
    const QString title = entry.value(QStringLiteral("session_title")).toString();
    return title.isEmpty() ? QStringLiteral("Untitled conversation") : title;
}

QString device(const QJsonObject& entry, const QHash<QString, QString>& names) {
    const QString name = names.value(entry.value(QStringLiteral("device_id")).toString());
    return name.isEmpty() ? QStringLiteral("Unknown device") : name;
}

} // namespace remote_activity

namespace {

// Lucide ISC path data for the kinds of request.
constexpr auto sendIcon =
    R"(<path d="M14.536 21.686a.5.5 0 0 0 .937-.024l6.5-19a.496.496 0 0 0-.635-.635l-19 6.5a.5.5 0 0 0-.024.937l7.93 3.18a2 2 0 0 1 1.112 1.11z"/><path d="m21.854 2.147-10.94 10.939"/>)";
constexpr auto stopIcon =
    R"(<circle cx="12" cy="12" r="10"/><rect x="9" y="9" width="6" height="6" rx="1"/>)";
constexpr auto branchIcon =
    R"(<line x1="6" x2="6" y1="3" y2="15"/><circle cx="18" cy="6" r="3"/><circle cx="6" cy="18" r="3"/><path d="M18 9a9 9 0 0 1-9 9"/>)";
constexpr auto trashIcon =
    R"(<path d="M3 6h18"/><path d="M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6"/><path d="M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2"/><line x1="10" x2="10" y1="11" y2="17"/><line x1="14" x2="14" y1="11" y2="17"/>)";
constexpr auto slidersIcon =
    R"(<line x1="21" x2="14" y1="4" y2="4"/><line x1="10" x2="3" y1="4" y2="4"/><line x1="21" x2="12" y1="12" y2="12"/><line x1="8" x2="3" y1="12" y2="12"/><line x1="21" x2="16" y1="20" y2="20"/><line x1="12" x2="3" y1="20" y2="20"/><line x1="14" x2="14" y1="2" y2="6"/><line x1="8" x2="8" y1="10" y2="14"/><line x1="16" x2="16" y1="18" y2="22"/>)";
constexpr auto checkIcon = R"(<path d="M20 6 9 17l-5-5"/>)";
constexpr auto terminalIcon =
    R"(<polyline points="4 17 10 11 4 5"/><line x1="12" x2="20" y1="19" y2="19"/>)";
constexpr auto closeIcon = R"(<path d="M18 6 6 18"/><path d="m6 6 12 12"/>)";

QByteArray actionIcon(const QString& kind) {
    if (kind == QStringLiteral("session.start_turn") || kind == QStringLiteral("session.steer"))
        return sendIcon;
    if (kind == QStringLiteral("session.interrupt") ||
        kind == QStringLiteral("session.task.stop") || kind == QStringLiteral("atlas.detach"))
        return stopIcon;
    if (kind == QStringLiteral("session.create") || kind == QStringLiteral("session.fork") ||
        kind == QStringLiteral("session.worktree.switch"))
        return branchIcon;
    if (kind == QStringLiteral("session.delete"))
        return trashIcon;
    if (kind.endsWith(QStringLiteral(".apply")) || kind == QStringLiteral("session.rename"))
        return slidersIcon;
    if (kind == QStringLiteral("approval.resolve") || kind == QStringLiteral("question.answer"))
        return checkIcon;
    return terminalIcon;
}

QLabel* label(const QString& text, const char* role, QWidget* parent = nullptr) {
    auto* value = new QLabel(text, parent);
    value->setProperty("remoteRole", role);
    return value;
}

// Draws a Lucide glyph in the label's style-sheet colour.
class GlyphLabel final : public QLabel {
  public:
    GlyphLabel(QByteArray body, QWidget* parent = nullptr)
        : QLabel(parent), body_(std::move(body)) {}

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        lucide_icon::paint(painter, body_, palette().color(QPalette::WindowText), rect());
    }

  private:
    QByteArray body_;
};

// Keeps its full text and shows as much as fits, ending in an ellipsis.
class ElidedLabel final : public QLabel {
  public:
    ElidedLabel(QString text, const char* role, QWidget* parent = nullptr)
        : QLabel(parent), full_(std::move(text)) {
        setProperty("remoteRole", role);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
        setText(full_);
    }
    [[nodiscard]] bool elided() const { return text() != full_; }
    // As wide as the whole text, and able to shrink to nothing.
    [[nodiscard]] QSize sizeHint() const override {
        return {fontMetrics().horizontalAdvance(full_), QLabel::sizeHint().height()};
    }
    [[nodiscard]] QSize minimumSizeHint() const override {
        return {0, QLabel::minimumSizeHint().height()};
    }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QLabel::resizeEvent(event);
        setText(fontMetrics().elidedText(full_, Qt::ElideRight, width()));
    }

  private:
    QString full_;
};

// One audited request. Rows take keyboard focus (Tab, or the arrow keys between rows) and are
// read as a list item named with the whole entry; a focused or hovered row shows the whole
// entry when any of it is cut short.
class ActivityRow final : public QFrame {
  public:
    ActivityRow(const QJsonObject& entry, const QHash<QString, QString>& names,
                const bool showDevice, const QDateTime& now, QWidget* parent = nullptr)
        : QFrame(parent) {
        setProperty("remoteRole", QStringLiteral("activityRow"));
        setProperty("remoteCommandId", entry.value(QStringLiteral("command_id")).toString());
        setFocusPolicy(Qt::StrongFocus);
        chrome_geometry::fixedHeight(this, 56);
        const QString kind = entry.value(QStringLiteral("kind")).toString();
        const auto result =
            remote_activity::outcome(entry.value(QStringLiteral("result_code")).toString());
        const QString action = remote_activity::action(kind);
        const QString time = remote_activity::when(
            QDateTime::fromString(entry.value(QStringLiteral("recorded_at")).toString(),
                                  Qt::ISODateWithMs),
            now);
        const QString device = remote_activity::device(entry, names);
        const QString target = remote_activity::target(entry);
        QStringList detail;
        if (!result.reason.isEmpty())
            detail.append(result.reason);
        if (showDevice)
            detail.append(device);
        if (!target.isEmpty())
            detail.append(target);
        full_ = QStringLiteral("%1, %2. %3%4. %5%6.")
                    .arg(action, time, result.text,
                         result.reason.isEmpty() ? QString() : QStringLiteral(": ") + result.reason,
                         device, target.isEmpty() ? QString() : QStringLiteral(", ") + target);
        setAccessibleName(full_);

        auto* layout = new QHBoxLayout(this);
        chrome_geometry::margins(layout, 18, 0, 18, 0);
        chrome_geometry::spacing(layout, 16);
        auto* icon = new GlyphLabel(actionIcon(kind), this);
        icon->setProperty("remoteRole", QStringLiteral("activityIcon"));
        chrome_geometry::fixedSize(icon, 16, 16);
        layout->addWidget(icon);
        auto* copy = new QVBoxLayout;
        chrome_geometry::spacing(copy, 4);
        copy->addStretch();
        auto* titleLine = new QHBoxLayout;
        chrome_geometry::spacing(titleLine, 8);
        title_ = new ElidedLabel(action, "activityTitle", this);
        titleLine->addWidget(title_);
        // The time follows the title and stays whole; the title gives way first.
        auto* timeLabel = label(time, "activityTime", this);
        timeLabel->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        titleLine->addWidget(timeLabel, 0, Qt::AlignBaseline);
        titleLine->addStretch(1);
        copy->addLayout(titleLine);
        detail_ = new ElidedLabel(detail.join(QStringLiteral(" · ")), "activityDetail", this);
        copy->addWidget(detail_);
        copy->addStretch();
        layout->addLayout(copy, 1);
        auto* badge = label(result.text, "badge", this);
        badge->setProperty("remoteTone", result.tone);
        badge->setAlignment(Qt::AlignCenter);
        badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        layout->addWidget(badge);
        for (QWidget* child : findChildren<QWidget*>())
            child->setAccessibleName(QString());
    }

    [[nodiscard]] bool cut() const { return title_->elided() || detail_->elided(); }
    [[nodiscard]] QString fullText() const { return full_; }

  protected:
    void resizeEvent(QResizeEvent* event) override {
        QFrame::resizeEvent(event);
        setToolTip(cut() ? full_ : QString());
    }
    void focusInEvent(QFocusEvent* event) override {
        QFrame::focusInEvent(event);
        for (QWidget* ancestor = parentWidget(); ancestor; ancestor = ancestor->parentWidget()) {
            if (auto* scroll = qobject_cast<QScrollArea*>(ancestor)) {
                scroll->ensureWidgetVisible(this, 0, 0);
                break;
            }
        }
        // Keyboard focus has no pointer to rest on the row, so the full text shows at once.
        if (cut() && event->reason() != Qt::MouseFocusReason)
            QToolTip::showText(mapToGlobal(QPoint(0, height())), full_, this);
    }
    void focusOutEvent(QFocusEvent* event) override {
        QFrame::focusOutEvent(event);
        QToolTip::hideText();
    }
    void keyPressEvent(QKeyEvent* event) override {
        if (event->key() == Qt::Key_Down || event->key() == Qt::Key_Up) {
            focusNextPrevChild(event->key() == Qt::Key_Down);
            return;
        }
        QFrame::keyPressEvent(event);
    }

  private:
    QString full_;
    ElidedLabel* title_{nullptr};
    ElidedLabel* detail_{nullptr};
};

// Rows are list items and their panels lists to assistive technology.
QAccessibleInterface* activityAccessible(const QString& key, QObject* object) {
    if (key != QLatin1String("QFrame") || !object->isWidgetType())
        return nullptr;
    const QString role = object->property("remoteRole").toString();
    if (role == QStringLiteral("activityRow"))
        return new QAccessibleWidget(static_cast<QWidget*>(object), QAccessible::ListItem);
    if (role == QStringLiteral("activityList"))
        return new QAccessibleWidget(static_cast<QWidget*>(object), QAccessible::List);
    return nullptr;
}

void clearLayout(QVBoxLayout* layout) {
    while (QLayoutItem* item = layout->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
}

QLabel* emptyLabel(const QString& text) {
    auto* value = label(text, "emptyText");
    value->setWordWrap(true);
    value->setAlignment(Qt::AlignCenter);
    chrome_geometry::minimumHeight(value, 96);
    return value;
}

} // namespace

RemoteActivitySection::RemoteActivitySection(remote::DaemonClient* daemon, QWidget* parent)
    : QWidget(parent), daemon_(daemon) {
    static const bool accessibleRows = [] {
        QAccessible::installFactory(activityAccessible);
        return true;
    }();
    Q_UNUSED(accessibleRows);
    setObjectName(QStringLiteral("remoteActivitySection"));
    auto* layout = new QVBoxLayout(this);
    chrome_geometry::margins(layout, 0, 0, 0, 0);
    chrome_geometry::spacing(layout, 12);
    auto* header = new QHBoxLayout;
    header->addWidget(label(QStringLiteral("Activity from paired devices"), "sectionTitle"));
    header->addStretch();
    header->addWidget(label(QStringLiteral("Last 90 days"), "sectionCount"));
    layout->addLayout(header);

    auto* panel = new QFrame(this);
    panel->setObjectName(QStringLiteral("remoteActivityPanel"));
    panel->setProperty("remoteRole", QStringLiteral("activityList"));
    panel->setAccessibleName(QStringLiteral("Activity from paired devices"));
    rows_ = new QVBoxLayout(panel);
    chrome_geometry::margins(rows_, 0, 0, 0, 0);
    chrome_geometry::spacing(rows_, 0);
    layout->addWidget(panel);

    auto* footer = new QHBoxLayout;
    auto* note = label(QStringLiteral("Recorded on this Mac as each request finishes. Reading "
                                      "files and transcripts is not listed."),
                       "trustNote");
    note->setWordWrap(true);
    footer->addWidget(note, 1);
    showAll_ = new QPushButton(QStringLiteral("Show all"), this);
    showAll_->setObjectName(QStringLiteral("remoteActivityShowAll"));
    showAll_->setProperty("remoteTertiary", true);
    showAll_->hide();
    connect(showAll_, &QPushButton::clicked, this, &RemoteActivitySection::showAll);
    footer->addWidget(showAll_);
    layout->addLayout(footer);
    render({});
}

void RemoteActivitySection::setDeviceNames(const QHash<QString, QString>& names) {
    names_ = names;
    render(entries_);
}

void RemoteActivitySection::refresh(const bool force) {
    if (requestInFlight_ || !daemon_ ||
        (!force && lastRefresh_.isValid() && lastRefresh_.elapsed() < 15'000))
        return;
    requestInFlight_ = true;
    lastRefresh_.start();
    const quint64 generation = ++generation_;
    // One more than is shown tells whether there are older entries.
    daemon_->call(
        QStringLiteral("remote.audit"), {{QStringLiteral("limit"), shownRows + 1}}, {}, -1,
        [self = QPointer(this), generation](const QJsonObject& result, const QString& error) {
            if (self)
                self->received(generation, result, error);
        });
}

void RemoteActivitySection::received(const quint64 generation, const QJsonObject& result,
                                     const QString& error) {
    // A reply asked for before sign-out belongs to the account that signed out.
    if (generation != generation_)
        return;
    requestInFlight_ = false;
    if (!error.isEmpty()) {
        render({});
        empty_->setText(QStringLiteral("Activity is temporarily unavailable."));
        return;
    }
    render(result.value(QStringLiteral("entries")).toArray());
}

void RemoteActivitySection::clear() {
    ++generation_;
    requestInFlight_ = false;
    lastRefresh_.invalidate();
    names_.clear();
    render({});
}

void RemoteActivitySection::render(const QJsonArray& entries) {
    // Rows are rebuilt on every refresh; the one a keyboard or screen reader user is on keeps
    // focus, or the row now in its place when its entry is gone.
    QWidget* focused = QApplication::focusWidget();
    const int focusedIndex = focused ? rows_->indexOf(focused) : -1;
    const QString focusedCommand =
        focusedIndex >= 0 ? focused->property("remoteCommandId").toString() : QString();
    entries_ = entries;
    clearLayout(rows_);
    empty_ = nullptr;
    showAll_->setVisible(entries.size() > shownRows);
    if (entries.isEmpty()) {
        empty_ = emptyLabel(QStringLiteral("No activity yet. Messages, new conversations and "
                                           "other changes a paired device makes on this Mac "
                                           "appear here."));
        empty_->setObjectName(QStringLiteral("remoteActivityEmpty"));
        rows_->addWidget(empty_);
        return;
    }
    QJsonArray shown;
    for (qsizetype index = 0; index < qMin<qsizetype>(entries.size(), shownRows); ++index)
        shown.append(entries.at(index));
    fillRows(rows_, shown, true);
    if (focusedIndex < 0)
        return;
    // The layout shows new rows later; a hidden row cannot take focus.
    QWidget* next = nullptr;
    for (int index = 0; index < rows_->count(); ++index) {
        QWidget* row = rows_->itemAt(index)->widget();
        row->show();
        if (!next && row->property("remoteCommandId").toString() == focusedCommand)
            next = row;
    }
    if (!next)
        next = rows_->itemAt(qMin(focusedIndex, rows_->count() - 1))->widget();
    next->setFocus(Qt::OtherFocusReason);
}

void RemoteActivitySection::fillRows(QVBoxLayout* rows, const QJsonArray& entries,
                                     const bool showDevice) const {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    for (qsizetype index = 0; index < entries.size(); ++index) {
        auto* row = new ActivityRow(entries.at(index).toObject(), names_, showDevice, now);
        row->setProperty("remoteFirst", index == 0);
        rows->addWidget(row);
    }
}

void RemoteActivitySection::showAll() {
    runDialog(QStringLiteral("Activity from paired devices"), {}, {}, {});
}

void RemoteActivitySection::showDevice(const QString& deviceId, const QString& name,
                                       std::function<void()> revoke) {
    runDialog(name, deviceId, QStringLiteral("KetPlus App · Paired with this Mac"),
              std::move(revoke));
}

void RemoteActivitySection::runDialog(const QString& title, const QString& deviceId,
                                      const QString& status, std::function<void()> revoke) {
    QDialog dialog(this);
    dialog.setObjectName(QStringLiteral("remoteActivityDialog"));
    dialog.setProperty("remoteRole", QStringLiteral("modal"));
    dialog.setWindowTitle(title);
    dialog.setWindowModality(Qt::WindowModal);
    chrome_geometry::fixedWidth(&dialog, 560);
    auto* root = new QVBoxLayout(&dialog);
    chrome_geometry::margins(root, 0, 0, 0, 0);
    chrome_geometry::spacing(root, 0);

    auto* header = new QFrame(&dialog);
    header->setProperty("remoteRole", QStringLiteral("modalHeader"));
    auto* headerLayout = new QHBoxLayout(header);
    chrome_geometry::margins(headerLayout, 22, 14, 14, 14);
    headerLayout->addWidget(label(title, "modalTitle"), 1);
    auto* close = new QPushButton(header);
    close->setProperty("remoteTertiary", true);
    close->setAccessibleName(QStringLiteral("Close"));
    close->setIcon(QIcon(
        lucide_icon::pixmap(closeIcon, palette().color(QPalette::WindowText), QSize(16, 16))));
    chrome_geometry::fixedSize(close, 30, 30);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    headerLayout->addWidget(close);
    root->addWidget(header);

    auto* body = new QWidget(&dialog);
    body->setProperty("remoteRole", QStringLiteral("modalBody"));
    auto* bodyLayout = new QVBoxLayout(body);
    chrome_geometry::margins(bodyLayout, 22, 18, 22, 20);
    chrome_geometry::spacing(bodyLayout, 12);
    if (!status.isEmpty())
        bodyLayout->addWidget(label(status, "deviceMeta"));
    auto* scroll = new QScrollArea(body);
    scroll->setObjectName(QStringLiteral("remoteActivityDialogScroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto* list = new QFrame;
    list->setProperty("remoteRole", QStringLiteral("activityList"));
    list->setAccessibleName(title);
    auto* rows = new QVBoxLayout(list);
    chrome_geometry::margins(rows, 0, 0, 0, 0);
    chrome_geometry::spacing(rows, 0);
    rows->addWidget(emptyLabel(QStringLiteral("Loading activity…")));
    rows->addStretch();
    scroll->setWidget(list);
    // Six rows show at once; the rest scroll.
    chrome_geometry::fixedHeight(scroll, 56 * 6);
    bodyLayout->addWidget(scroll);
    auto* limitNote = label(QStringLiteral("Showing the newest %1.").arg(dialogLimit), "trustNote");
    limitNote->hide();
    bodyLayout->addWidget(limitNote);
    root->addWidget(body);

    auto* footer = new QFrame(&dialog);
    footer->setProperty("remoteRole", QStringLiteral("modalFooter"));
    auto* actions = new QHBoxLayout(footer);
    chrome_geometry::margins(actions, 22, 13, 22, 13);
    chrome_geometry::spacing(actions, 8);
    actions->addStretch();
    bool revoking = false;
    if (revoke) {
        auto* revokeButton = new QPushButton(QStringLiteral("Revoke access"), footer);
        revokeButton->setObjectName(QStringLiteral("remoteActivityRevoke"));
        revokeButton->setProperty("remoteDanger", true);
        connect(revokeButton, &QPushButton::clicked, &dialog, [&dialog, &revoking] {
            revoking = true;
            dialog.accept();
        });
        actions->addWidget(revokeButton);
    }
    auto* done = new QPushButton(QStringLiteral("Close"), footer);
    done->setObjectName(QStringLiteral("remoteActivityClose"));
    done->setProperty("remotePrimary", true);
    connect(done, &QPushButton::clicked, &dialog, &QDialog::accept);
    actions->addWidget(done);
    root->addWidget(footer);

    QJsonObject payload{{QStringLiteral("limit"), dialogLimit}};
    if (!deviceId.isEmpty())
        payload.insert(QStringLiteral("device_id"), deviceId);
    const QPointer<QFrame> guard(list);
    if (daemon_) {
        daemon_->call(
            QStringLiteral("remote.audit"), payload, {}, -1,
            [this, guard, rows, limitNote, deviceId](const QJsonObject& result,
                                                     const QString& error) {
                if (!guard)
                    return;
                clearLayout(rows);
                const QJsonArray entries = result.value(QStringLiteral("entries")).toArray();
                if (!error.isEmpty())
                    rows->addWidget(
                        emptyLabel(QStringLiteral("Activity is temporarily unavailable.")));
                else if (entries.isEmpty())
                    rows->addWidget(emptyLabel(
                        deviceId.isEmpty()
                            ? QStringLiteral("No activity yet.")
                            : QStringLiteral("Nothing this device asked for has changed this "
                                             "Mac in the last 90 days.")));
                else
                    fillRows(rows, entries, deviceId.isEmpty());
                rows->addStretch();
                limitNote->setVisible(entries.size() >= dialogLimit);
            });
    }
    dialog.exec();
    if (revoking && revoke)
        revoke();
}

} // namespace ketplus

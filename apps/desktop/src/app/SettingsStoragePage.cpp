#include "app/SettingsStoragePage.h"
#include "KetButton.h"
#include "KetSelect.h"
#include "KetToggle.h"
#include "app/SettingsRows.h"

#include <QAbstractButton>
#include <QComboBox>
#include <QLabel>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>
namespace ketplus {
namespace {
QLabel* label(const QString& text, const QString& name, QWidget* parent) {
    auto* result = new QLabel(text, parent);
    result->setObjectName(name);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
bool integer(const QJsonValue& value, qint64 maximum = 9007199254740991LL) {
    return value.isDouble() && value.toDouble() >= 0 && value.toDouble() <= maximum &&
           value.toDouble() == value.toInteger(-1);
}
ket::Select* select(const QString& name, const QString& accessible, QWidget* parent) {
    auto* result = new ket::Select(parent);
    result->setObjectName(name);
    result->setAccessibleName(accessible);
    result->setAccessibleDescription(accessible +
                                     SettingsStoragePage::tr(". Changes save immediately."));
    result->setMinimumHeight(32);
    return result;
}
} // namespace
SettingsStoragePage::SettingsStoragePage(Requester requester, QWidget* parent)
    : QWidget(parent), requester_(std::move(requester)) {
    setObjectName("settingsWorkspaceStorage");
    timeout_ = new QTimer(this);
    timeout_->setObjectName("storageRequestTimeout");
    timeout_->setSingleShot(true);
    timeout_->setInterval(20'000);
    connect(timeout_, &QTimer::timeout, this, [this] {
        ++generation_; // A timed-out save may have committed; late replies cannot replace a reload.
        fail(tr("The daemon did not respond in time. Reload before making another change."));
    });
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 24);
    layout->setSpacing(0);
    layout->addWidget(label(tr("Storage"), "settingsWorkspaceSectionTitle", this));
    layout->addSpacing(7);
    layout->addWidget(label(tr("KetPlus keeps every conversation in a local journal so "
                               "transcripts, remote devices and restarts can replay them. Choose "
                               "how long it keeps full detail and how large it may grow."),
                            "settingsWorkspaceIntro", this));
    layout->addSpacing(20);
    buildOverview(layout);

    layout->addSpacing(20);
    layout->addWidget(settings_rows::divider(this));
    layout->addSpacing(20);
    layout->addWidget(label(tr("Retention"), "settingsWorkspaceSubheading", this));
    layout->addSpacing(14);
    days_ = select("storageDetailDays", tr("Keep full detail for"), this);
    days_->addItem(tr("7 days"), 7);
    days_->addItem(tr("30 days"), 30);
    days_->addItem(tr("90 days"), 90);
    days_->addItem(tr("Forever"), 0);
    days_->setCurrentIndex(-1);
    layout->addWidget(settings_rows::field(
        tr("Keep full detail for"),
        tr("While idle, trim supported older tool details. Messages, running turns and the "
           "latest 20 turns stay protected. Changes save immediately."),
        days_, this));
    layout->addSpacing(20);
    sizeRow_ = new QWidget(this);
    auto* sizeLayout = new QVBoxLayout(sizeRow_);
    sizeLayout->setContentsMargins(0, 0, 0, 20);
    sizeLayout->setSpacing(8);
    limit_ = select("storageSizeLimit", tr("Database size target"), sizeRow_);
    for (const qint64 gb : {1, 2, 5, 10})
        limit_->addItem(tr("%1 GB").arg(gb), gb * 1'000'000'000LL);
    limit_->addItem(tr("No limit"), qint64(0));
    sizeLayout->addWidget(settings_rows::field(
        tr("Database size target"),
        tr("While idle, trim eligible detail earlier to stay under this soft SQLite-page target. "
           "It excludes media; protected content can remain above it. Forever and "
           "automatic-off preserve detail."),
        limit_, sizeRow_));
    sizeStatus_ = label({}, "storageSizeStatus", sizeRow_);
    sizeStatus_->setProperty("settingsHelp", true);
    sizeLayout->addWidget(sizeStatus_);
    sizeRow_->hide();
    layout->addWidget(sizeRow_);

    optionsGroup_ = new QWidget(this);
    auto* optionsLayout = new QVBoxLayout(optionsGroup_);
    optionsLayout->setContentsMargins(0, 0, 0, 20);
    optionsLayout->setSpacing(20);
    images_ = select("storageGeneratedImages", tr("Generated images"), optionsGroup_);
    images_->addItem(tr("Keep full image"), QStringLiteral("full"));
    images_->addItem(tr("Keep preview"), QStringLiteral("preview"));
    images_->addItem(tr("Caption only"), QStringLiteral("caption"));
    optionsLayout->addWidget(settings_rows::field(
        tr("Generated images"),
        tr("After the detail period, keep the full image, a 512 px preview, or only its "
           "caption."),
        images_, optionsGroup_));
    archived_ = select("storageArchivedHistory", tr("Archived conversations"), optionsGroup_);
    archived_->addItem(tr("30 days"), 30);
    archived_->addItem(tr("90 days"), 90);
    archived_->addItem(tr("1 year"), 365);
    archived_->addItem(tr("Never"), 0);
    optionsLayout->addWidget(settings_rows::field(
        tr("Archived conversations"),
        tr("Delete the history of a conversation this long after it is archived."), archived_,
        optionsGroup_));
    warn_ =
        new ket::Checkbox(tr("Warn me when the journal passes 80% of its limit"), optionsGroup_);
    warn_->setObjectName("storageWarnNearLimit");
    optionsLayout->addWidget(warn_);
    optionsGroup_->hide();
    layout->addWidget(optionsGroup_);
    automatic_ = new ket::Checkbox(tr("Trim older detail automatically while idle"), this);
    automatic_->setObjectName("storageAutomaticDetail");
    automatic_->setAccessibleName(automatic_->text());
    layout->addWidget(automatic_);

    buildJobs(layout);
    layout->addSpacing(14);
    status_ = label({}, "storageStatus", this);
    layout->addWidget(status_);
    refresh_ = new ket::Button(tr("Reload settings"), ket::ButtonVariant::Secondary, this);
    refresh_->setObjectName("storageReload");
    refresh_->setMinimumHeight(32);
    refresh_->hide();
    layout->addSpacing(8);
    layout->addWidget(refresh_, 0, Qt::AlignLeft);
    layout->addStretch(1);
    connect(days_, &QComboBox::activated, this, [this] { saveDetail(); });
    connect(automatic_, &QAbstractButton::clicked, this, [this] { saveDetail(); });
    connect(limit_, &QComboBox::activated, this, [this] { saveSize(); });
    connect(images_, &QComboBox::activated, this, [this] { saveOptions(); });
    connect(archived_, &QComboBox::activated, this, [this] { saveOptions(); });
    connect(warn_, &QAbstractButton::clicked, this, [this] { saveOptions(); });
    connect(refresh_, &QAbstractButton::clicked, this, &SettingsStoragePage::refresh);
    setBusy(true);
    QTimer::singleShot(0, this, &SettingsStoragePage::refresh);
}
void SettingsStoragePage::setBusy(bool busy) {
    busy_ = busy;
    days_->setEnabled(!busy && revision_ >= 0);
    automatic_->setEnabled(!busy && revision_ >= 0);
    refresh_->setEnabled(!busy);
    limit_->setEnabled(!busy && sizeRevision_ >= 0);
    for (QWidget* control : {static_cast<QWidget*>(images_), static_cast<QWidget*>(archived_),
                             static_cast<QWidget*>(warn_)})
        control->setEnabled(!busy && optionsRevision_ >= 0);
    compact_->setEnabled(!busy && sizeRevision_ >= 0 && optionsRevision_ >= 0 &&
                         !compactionRunning_);
    export_->setEnabled(!busy && optionsRevision_ >= 0 && !exportRunning_);
}
void SettingsStoragePage::refresh() {
    revision_ = sizeRevision_ = optionsRevision_ = -1;
    refresh_->hide();
    status_->setText(tr("Loading storage settings…"));
    request("read", {});
}
void SettingsStoragePage::saveDetail() {
    if (busy_ || revision_ < 0)
        return;
    status_->setText(tr("Saving…"));
    request("apply",
            {{"expected_revision", revision_},
             {"policy", QJsonObject{{"full_detail_days", days_->currentData().toInt()},
                                    {"automatic_detail_retention", automatic_->isChecked()}}}});
}
void SettingsStoragePage::saveSize() {
    if (busy_ || sizeRevision_ < 0)
        return;
    status_->setText(tr("Saving…"));
    // The warning threshold has no control of its own; it keeps the saved value.
    request("size.apply",
            {{"expected_revision", sizeRevision_},
             {"policy", QJsonObject{{"limit_bytes", limit_->currentData().toLongLong()},
                                    {"warning_percent", warningPercent_}}}});
}
void SettingsStoragePage::saveOptions() {
    if (busy_ || optionsRevision_ < 0)
        return;
    status_->setText(tr("Saving…"));
    request("options.apply",
            {{"expected_revision", optionsRevision_},
             {"options", QJsonObject{{"archived_history_days", archived_->currentData().toInt()},
                                     {"generated_images", images_->currentData().toString()},
                                     {"warn_near_limit", warn_->isChecked()}}}});
}
void SettingsStoragePage::request(const QString& operation, const QJsonObject& payload) {
    setBusy(true);
    const auto generation = ++generation_;
    timeout_->start();
    if (!requester_) {
        receive(operation, {},
                tr("Connect to an updated local KetPlus daemon, then reload settings."));
        return;
    }
    const QPointer<SettingsStoragePage> guard(this);
    requester_(operation, payload,
               [guard, generation, operation](const QJsonObject& result, const QString& error) {
                   if (guard && guard->generation_ == generation)
                       guard->receive(operation, result, error);
               });
}
void SettingsStoragePage::receive(const QString& operation, const QJsonObject& result,
                                  const QString& error) {
    timeout_->stop();
    if (!error.isEmpty() || !result.value("ok").toBool()) {
        if (result.value("error_code").toString() == "policy_conflict")
            fail(tr("Settings changed in another window. Reload before making another change."));
        else
            fail(tr("Could not confirm storage settings. %1 Reload before making another change.")
                     .arg(error.isEmpty() ? result.value("error_message").toString() : error));
        return;
    }
    const bool saving = operation.endsWith("apply");
    saved_ = saved_ || saving;
    if (operation.startsWith("size."))
        receiveSize(result, saving);
    else if (operation.startsWith("options."))
        receiveOptions(result, saving);
    else
        receiveDetail(result, saving);
}
void SettingsStoragePage::receiveDetail(const QJsonObject& result, bool saving) {
    const auto policy = result.value("policy").toObject();
    const auto days = policy.value("full_detail_days");
    const int index = integer(days, 90) ? days_->findData(days.toInt()) : -1;
    if (!integer(result.value("revision")) || policy.size() != 2 || index < 0 ||
        !policy.value("automatic_detail_retention").isBool()) {
        fail(tr("The daemon returned unsupported storage settings. Update it and reload."));
        return;
    }
    if (saving &&
        (result.value("revision").toInteger() != revision_ + 1 ||
         days.toInt() != days_->currentData().toInt() ||
         policy.value("automatic_detail_retention").toBool() != automatic_->isChecked())) {
        fail(tr("The daemon did not confirm this change. Reload to check the saved settings."));
        return;
    }
    revision_ = result.value("revision").toInteger();
    days_->setCurrentIndex(index);
    automatic_->setChecked(policy.value("automatic_detail_retention").toBool());
    detailDays_ = days.toInt();
    showMeasurements(result);
    // An older daemon has only the detail policy; the page keeps what it supports.
    optionsSupported_ = result.value("storage_options_supported").toBool();
    optionsGroup_->setVisible(optionsSupported_);
    if (result.value("size_policy_supported").toBool()) {
        request("size.read", {});
        return;
    }
    sizeRow_->hide();
    finish();
}
void SettingsStoragePage::receiveSize(const QJsonObject& result, bool saving) {
    const auto policy = result.value("policy").toObject();
    const auto limit = policy.value("limit_bytes");
    const auto warning = policy.value("warning_percent");
    const int index = integer(limit) ? limit_->findData(limit.toInteger()) : -1;
    if (!integer(result.value("revision")) || policy.size() != 2 || index < 0 ||
        !integer(warning, 100) || warning.toInt() < 1 ||
        (saving && (result.value("revision").toInteger() != sizeRevision_ + 1 ||
                    limit.toInteger() != limit_->currentData().toLongLong()))) {
        fail(tr(
            "The daemon did not confirm the database target. Reload to check the saved settings."));
        return;
    }
    sizeRevision_ = result.value("revision").toInteger();
    warningPercent_ = warning.toInt();
    limitBytes_ = limit.toInteger();
    {
        const QSignalBlocker blocker(limit_);
        limit_->setCurrentIndex(index);
    }
    warn_->setText(tr("Warn me when the journal passes %1% of its limit").arg(warningPercent_));
    sizeRow_->show();
    const auto state = result.value("status").toObject();
    if (limitBytes_ == 0)
        sizeStatus_->setText(tr("No size target. Age-based detail preferences still apply."));
    else if (state.value("pressure_enabled").isBool() && !state.value("pressure_enabled").toBool())
        sizeStatus_->setText(tr("Inactive while detail is kept forever or automatic trimming is "
                                "off."));
    else
        sizeStatus_->setText(tr("Warning threshold: %1%.").arg(warningPercent_));
    showMeasurements(result);
    showCompaction(result.value("compaction").toObject());
    if (optionsSupported_) {
        request("options.read", {});
        return;
    }
    finish();
}
void SettingsStoragePage::receiveOptions(const QJsonObject& result, bool saving) {
    const auto options = result.value("options").toObject();
    const int images = images_->findData(options.value("generated_images").toString());
    const auto days = options.value("archived_history_days");
    const int archived = integer(days, 365) ? archived_->findData(days.toInt()) : -1;
    if (!integer(result.value("revision")) || options.size() != 3 || images < 0 || archived < 0 ||
        !options.value("warn_near_limit").isBool() ||
        (saving && (result.value("revision").toInteger() != optionsRevision_ + 1 ||
                    images != images_->currentIndex() || archived != archived_->currentIndex() ||
                    options.value("warn_near_limit").toBool() != warn_->isChecked()))) {
        fail(tr("The daemon did not confirm the storage options. Reload to check the saved "
                "settings."));
        return;
    }
    optionsRevision_ = result.value("revision").toInteger();
    images_->setCurrentIndex(images);
    archived_->setCurrentIndex(archived);
    warn_->setChecked(options.value("warn_near_limit").toBool());
    if (result.value("status").isObject())
        showMeasurements(QJsonObject{{"status", result.value("status")}});
    finish();
    if (!saving)
        poll("breakdown.read", {}, [this](const QJsonObject& reply) { showBreakdown(reply); });
}
void SettingsStoragePage::finish() {
    status_->setText(saved_ ? tr("Saved. Changes apply while KetPlus is idle.") : QString());
    saved_ = false;
    setBusy(false);
}
void SettingsStoragePage::fail(const QString& message) {
    timeout_->stop();
    saved_ = false;
    revision_ = sizeRevision_ = optionsRevision_ = -1;
    days_->setCurrentIndex(-1);
    automatic_->setChecked(false);
    sizeStatus_->setText(tr("Database target status unavailable. Reload to check."));
    fileSize_->setText(tr("Unavailable"));
    status_->setText(message);
    refresh_->show();
    setBusy(false);
}
} // namespace ketplus

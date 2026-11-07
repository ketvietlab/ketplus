#include "app/SettingsStoragePage.h"
#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QPushButton>
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
        receive({}, tr("The daemon did not respond in time."), false);
    });
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 8, 24);
    layout->setSpacing(12);
    layout->addWidget(label(tr("Storage"), "settingsWorkspaceSectionTitle", this));
    layout->addWidget(label(tr("Keep conversations on this computer and choose how long to retain "
                               "their full tool details. Changes save immediately."),
                            "settingsWorkspaceIntro", this));
    layout->addSpacing(8);
    usage_ = label(tr("Loading journal usage…"), "storageUsage", this);
    layout->addWidget(usage_);
    layout->addWidget(
        label(tr("File sizes include the SQLite database, write-ahead log and shared "
                 "memory file. Media, backups and other application files are excluded. "
                 "Measurements refresh when you reload or save."),
              "settingsWorkspaceIntro", this));
    layout->addSpacing(8);
    days_ = new QComboBox(this);
    days_->setObjectName("storageDetailDays");
    days_->setAccessibleName(tr("Keep full detail for"));
    days_->setAccessibleDescription(tr("Keep full detail for. Changes save immediately."));
    days_->addItem(tr("7 days"), 7);
    days_->addItem(tr("30 days"), 30);
    days_->addItem(tr("90 days"), 90);
    days_->addItem(tr("Forever"), 0);
    days_->setCurrentIndex(-1);
    days_->setMinimumHeight(32);
    auto* title = label(tr("Keep full detail for"), "settingsWorkspaceFieldLabel", this);
    title->setBuddy(days_);
    layout->addWidget(title);
    layout->addWidget(days_, 0, Qt::AlignLeft);
    automatic_ = new QCheckBox(tr("Trim older detail automatically while idle"), this);
    automatic_->setObjectName("storageAutomaticDetail");
    automatic_->setAccessibleName(automatic_->text());
    layout->addWidget(automatic_);
    layout->addWidget(
        label(tr("Messages, running turns and the latest 20 turns of each conversation "
                 "stay protected. Older command output, tool text and file diffs may "
                 "be shortened. Forever or turning this off preserves remaining detail; "
                 "it cannot restore detail already trimmed. Unused database pages can "
                 "still be reclaimed."),
              "settingsWorkspaceIntro", this));
    status_ = label(tr("Loading storage settings…"), "storageStatus", this);
    layout->addWidget(status_);
    refresh_ = new QPushButton(tr("Reload settings"), this);
    refresh_->setObjectName("storageReload");
    refresh_->setMinimumHeight(32);
    layout->addWidget(refresh_, 0, Qt::AlignLeft);
    layout->addStretch(1);
    connect(days_, &QComboBox::activated, this, [this] { save(); });
    connect(automatic_, &QCheckBox::clicked, this, [this] { save(); });
    connect(refresh_, &QPushButton::clicked, this, &SettingsStoragePage::refresh);
    setBusy(true);
    QTimer::singleShot(0, this, &SettingsStoragePage::refresh);
}
void SettingsStoragePage::setBusy(bool busy) {
    busy_ = busy;
    days_->setEnabled(!busy && revision_ >= 0);
    automatic_->setEnabled(!busy && revision_ >= 0);
    refresh_->setEnabled(!busy);
}
void SettingsStoragePage::refresh() {
    revision_ = -1;
    status_->setText(tr("Loading storage settings…"));
    request("read", {});
}
void SettingsStoragePage::save() {
    if (busy_ || revision_ < 0)
        return;
    status_->setText(tr("Saving…"));
    request("apply",
            {{"expected_revision", revision_},
             {"policy", QJsonObject{{"full_detail_days", days_->currentData().toInt()},
                                    {"automatic_detail_retention", automatic_->isChecked()}}}});
}
void SettingsStoragePage::request(const QString& operation, const QJsonObject& payload) {
    setBusy(true);
    const auto generation = ++generation_;
    timeout_->start();
    if (!requester_) {
        receive({}, tr("Connect to an updated local KetPlus daemon, then reload settings."), false);
        return;
    }
    const QPointer<SettingsStoragePage> guard(this);
    requester_(operation, payload,
               [guard, generation, saving = operation == "apply"](const QJsonObject& result,
                                                                  const QString& error) {
                   if (guard && guard->generation_ == generation)
                       guard->receive(result, error, saving);
               });
}
void SettingsStoragePage::receive(const QJsonObject& result, const QString& error, bool saving) {
    timeout_->stop();
    const auto fail = [this](const QString& message) {
        revision_ = -1;
        days_->setCurrentIndex(-1);
        automatic_->setChecked(false);
        usage_->setText(tr("Journal usage unavailable. Reload to check the current state."));
        status_->setText(message);
        setBusy(false);
    };
    if (!error.isEmpty() || !result.value("ok").toBool()) {
        if (result.value("error_code").toString() == "policy_conflict")
            fail(tr("Settings changed in another window. Reload before making another change."));
        else
            fail(tr("Could not confirm storage settings. %1 Reload before making another change.")
                     .arg(error.isEmpty() ? result.value("error_message").toString() : error));
        return;
    }
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
    const auto usage = result.value("usage").toObject();
    const auto total = usage.value("database_page_bytes");
    const auto free = usage.value("free_page_bytes");
    const auto kept = usage.value("kept_page_bytes");
    if (integer(total) && integer(free) && integer(kept) && free.toInteger() <= total.toInteger() &&
        kept.toInteger() == total.toInteger() - free.toInteger()) {
        const QLocale locale;
        usage_->setText(tr("Database pages: %1 · In use: %2 · Reclaimable: %3")
                            .arg(locale.formattedDataSize(total.toInteger()),
                                 locale.formattedDataSize(kept.toInteger()),
                                 locale.formattedDataSize(free.toInteger())));
    } else {
        usage_->setText(
            tr("SQLite page usage unavailable. Your retention settings are available."));
    }
    const auto disk = result.value("disk_usage").toObject();
    const auto fileBytes = disk.value("total_file_bytes");
    const auto databaseBytes = disk.value("database_file_bytes");
    const auto walBytes = disk.value("wal_file_bytes");
    const auto sharedBytes = disk.value("shared_memory_file_bytes");
    const auto available = disk.value("filesystem_available_bytes");
    const QLocale locale;
    QString measurements;
    if (integer(fileBytes) && integer(databaseBytes) && integer(walBytes) && integer(sharedBytes) &&
        fileBytes.toInteger() ==
            databaseBytes.toInteger() + walBytes.toInteger() + sharedBytes.toInteger()) {
        measurements =
            tr("SQLite files: %1 (write-ahead log: %2).")
                .arg(locale.formattedDataSize(fileBytes.toInteger(), 2, QLocale::DataSizeSIFormat),
                     locale.formattedDataSize(walBytes.toInteger(), 2, QLocale::DataSizeSIFormat));
    } else {
        measurements = tr("SQLite file sizes unavailable.");
    }
    if (integer(available)) {
        measurements +=
            tr(" Disk space available: %1.")
                .arg(locale.formattedDataSize(available.toInteger(), 2, QLocale::DataSizeSIFormat));
        if (available.toInteger() < 5'000'000'000LL)
            measurements +=
                tr(" Low disk space: less than 5 GB available. Journal writes may fail.");
    } else {
        measurements += tr(" Disk space available is unknown.");
    }
    usage_->setText(usage_->text() + "\n" + measurements);
    status_->setText(saving ? tr("Saved. Changes apply while KetPlus is idle.")
                            : tr("Settings are up to date."));
    setBusy(false);
}
} // namespace ketplus

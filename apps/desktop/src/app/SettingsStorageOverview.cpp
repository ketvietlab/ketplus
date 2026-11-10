// What the journal holds, and the jobs that take longer than a request: the measured parts,
// Compact now and Export (ADR 0108). Each polls the daemon while it runs.
#include "KetBanner.h"
#include "KetButton.h"
#include "KetIcons.h"
#include "KetMeter.h"
#include "app/SettingsRows.h"
#include "app/SettingsStoragePage.h"

#include <QAbstractButton>
#include <QDateTime>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLocale>
#include <QPointer>
#include <QTimer>
#include <QVBoxLayout>
namespace ketplus {
namespace {
QLabel* text(const QString& value, const QString& name, QWidget* parent) {
    auto* result = new QLabel(value, parent);
    result->setObjectName(name);
    result->setTextFormat(Qt::PlainText);
    result->setWordWrap(true);
    return result;
}
// The mock's sizes: decimal units, one decimal place for gigabytes.
QString bytesText(qint64 bytes) {
    if (bytes >= 1'000'000'000LL)
        return SettingsStoragePage::tr("%1 GB").arg(double(bytes) / 1e9, 0, 'f', 1);
    if (bytes >= 1'000'000LL)
        return SettingsStoragePage::tr("%1 MB").arg((bytes + 500'000) / 1'000'000);
    return SettingsStoragePage::tr("%1 KB").arg((qMax<qint64>(0, bytes) + 500) / 1000);
}
QString limitText(qint64 bytes) {
    return SettingsStoragePage::tr("%1 GB").arg(bytes / 1'000'000'000LL);
}
QString day(const QDateTime& time) {
    return QLocale().toString(time.toLocalTime().date(), QStringLiteral("d MMM yyyy"));
}
qint64 bytesOf(const QJsonObject& object, const char* key) {
    const auto value = object.value(QLatin1String(key));
    return value.isDouble() && value.toDouble() >= 0 ? value.toInteger(-1) : -1;
}
} // namespace

void SettingsStoragePage::buildOverview(QVBoxLayout* layout) {
    fileSize_ = text(tr("Loading…"), "storageFileSize", this);
    layout->addWidget(
        settings_rows::info(tr("SQLite files (including write-ahead log)"), fileSize_, this));
    layout->addSpacing(8);
    available_ = text(tr("Loading…"), "storageDiskAvailable", this);
    layout->addWidget(settings_rows::info(tr("Disk space available"), available_, this));
    layout->addSpacing(20);
    limitBanner_ = new ket::Banner(ket::BannerTone::Warning, {}, {}, this);
    limitBanner_->setObjectName("storageLimitBanner");
    connect(limitBanner_->setAction(tr("Compact now")), &QAbstractButton::clicked, this,
            &SettingsStoragePage::startCompaction);
    limitBanner_->hide();
    // Hidden banners take no space; two visible ones keep the mock's gap between them.
    auto* banners = new QVBoxLayout;
    banners->setSpacing(12);
    banners->addWidget(limitBanner_);
    diskBanner_ = new ket::Banner(ket::BannerTone::Warning, tr("Disk space is low"), {}, this);
    diskBanner_->setObjectName("storageDiskBanner");
    diskBanner_->hide();
    banners->addWidget(diskBanner_);
    layout->addLayout(banners);
    layout->addSpacing(20);
    journal_ = new ket::Meter(tr("Journal"), this);
    journal_->setObjectName("storageJournalMeter");
    layout->addWidget(journal_);
    layout->addSpacing(20);
    const QList<std::pair<QString, QString>> parts{
        {tr("Conversation history"), tr("Messages, tool calls and command output")},
        {tr("Generated images"), {}},
        {tr("Activity and usage index"), tr("Activity groups, usage samples")},
        {tr("Free space"), tr("Left by deleted data; compacting returns it to macOS")}};
    for (const auto& [name, caption] : parts) {
        auto* meter = new ket::Meter(name, this);
        meter->setObjectName("storagePartMeter");
        meter->setCaption(caption);
        meter->setValueText(tr("Measuring…"));
        meter->setFraction(0);
        parts_.append(meter);
        layout->addWidget(meter);
        layout->addSpacing(10);
    }
}

void SettingsStoragePage::buildJobs(QVBoxLayout* layout) {
    largestSection_ = new QWidget(this);
    auto* section = new QVBoxLayout(largestSection_);
    section->setContentsMargins(0, 20, 0, 0);
    section->setSpacing(0);
    section->addWidget(settings_rows::divider(largestSection_));
    section->addSpacing(20);
    section->addWidget(
        text(tr("Largest conversations"), "settingsWorkspaceSubheading", largestSection_));
    section->addSpacing(14);
    auto* rows = new QWidget(largestSection_);
    largest_ = new QVBoxLayout(rows);
    largest_->setContentsMargins(0, 0, 0, 0);
    largest_->setSpacing(8);
    section->addWidget(rows);
    largestSection_->hide();
    layout->addWidget(largestSection_);

    layout->addSpacing(20);
    layout->addWidget(settings_rows::divider(this));
    layout->addSpacing(20);
    lastCompaction_ = text(tr("Never"), "storageLastCompaction", this);
    layout->addWidget(settings_rows::info(tr("Last compaction"), lastCompaction_, this));
    layout->addSpacing(14);
    compacting_ = new ket::Meter(tr("Compacting"), this);
    compacting_->setObjectName("storageCompactionProgress");
    compacting_->hide();
    layout->addWidget(compacting_);
    auto* buttons = new QWidget(this);
    auto* row = new QHBoxLayout(buttons);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);
    compact_ = new ket::Button(tr("Compact now"), ket::ButtonVariant::Primary, buttons);
    compact_->setObjectName("storageCompactNow");
    export_ = new ket::Button(tr("Export conversations…"), ket::ButtonVariant::Secondary, buttons);
    export_->setObjectName("storageExport");
    export_->setIcon(ket::Icons::icon(u"download"));
    row->addWidget(compact_);
    row->addWidget(export_);
    row->addStretch(1);
    layout->addWidget(buttons);
    layout->addSpacing(10);
    auto* help = text(tr("Compacting trims old detail and returns free space to the disk while no "
                         "turn is running; a running turn keeps working and compaction waits "
                         "for it."),
                      "settingsWorkspaceHelp", this);
    layout->addWidget(help);
    jobStatus_ = text({}, "storageJobStatus", this);
    jobStatus_->setAccessibleName(tr("Compaction and export status"));
    layout->addSpacing(8);
    layout->addWidget(jobStatus_);
    connect(compact_, &QAbstractButton::clicked, this, &SettingsStoragePage::startCompaction);
    connect(export_, &QAbstractButton::clicked, this, &SettingsStoragePage::startExport);

    jobsTimer_ = new QTimer(this);
    jobsTimer_->setObjectName("storageJobsPoll");
    jobsTimer_->setInterval(1000);
    connect(jobsTimer_, &QTimer::timeout, this, [this] {
        if (compactionRunning_)
            poll("compaction.read", {}, [this](const QJsonObject& reply) {
                showCompaction(reply.value("compaction").toObject());
            });
        if (exportRunning_)
            poll("export.read", {}, [this](const QJsonObject& reply) {
                showExport(reply.value("export").toObject());
            });
        if (measuring_)
            poll("breakdown.read", {}, [this](const QJsonObject& reply) { showBreakdown(reply); });
        if (!compactionRunning_ && !exportRunning_ && !measuring_)
            jobsTimer_->stop();
    });
}

void SettingsStoragePage::poll(const QString& operation, const QJsonObject& payload,
                               const std::function<void(const QJsonObject&)>& done) {
    if (!requester_)
        return;
    const QPointer<SettingsStoragePage> guard(this);
    const auto generation = jobsGeneration_;
    requester_(operation, payload,
               [guard, generation, done](const QJsonObject& result, const QString& error) {
                   if (!guard || guard->jobsGeneration_ != generation)
                       return;
                   if (!error.isEmpty() || !result.value("ok").toBool()) {
                       guard->jobStatus_->setText(
                           error.isEmpty() ? result.value("error_message").toString() : error);
                       return;
                   }
                   done(result);
               });
}

void SettingsStoragePage::showMeasurements(const QJsonObject& result) {
    const auto disk = result.value("disk_usage").toObject();
    if (const auto files = bytesOf(disk, "total_file_bytes"); files >= 0)
        fileSize_->setText(bytesText(files));
    if (const auto available = bytesOf(disk, "filesystem_available_bytes"); available >= 0)
        available_->setText(bytesText(available));
    const auto usage = result.value("usage").toObject();
    if (bytesOf(usage, "database_page_bytes") >= 0) {
        databaseBytes_ = bytesOf(usage, "database_page_bytes");
        freeBytes_ = bytesOf(usage, "free_page_bytes");
    }
    const auto status = result.value("status").toObject();
    if (status.isEmpty())
        return;
    if (bytesOf(status, "database_page_bytes") >= 0)
        databaseBytes_ = bytesOf(status, "database_page_bytes");
    if (bytesOf(status, "limit_bytes") >= 0)
        limitBytes_ = bytesOf(status, "limit_bytes");
    const bool over = status.value("over_limit").toBool();
    const bool near = status.value("warning").toBool();
    limitBanner_->setVisible(over || near);
    if (over || near) {
        limitBanner_->setTitle(over ? tr("The journal is over its size limit")
                                    : tr("The journal is near its size limit"));
        QString message =
            tr("%1 on disk, limit %2.").arg(bytesText(databaseBytes_), limitText(limitBytes_));
        if (freeBytes_ > 0)
            message += QLatin1Char(' ') + tr("%1 of it is free space that compacting returns to "
                                             "macOS.")
                                              .arg(bytesText(freeBytes_));
        limitBanner_->setMessage(message);
    }
    const bool lowDisk = status.value("low_disk").toBool();
    diskBanner_->setVisible(lowDisk);
    if (lowDisk)
        diskBanner_->setMessage(
            tr("%1 available. Journal writes fail when the disk is full; compact the journal "
               "or free up space.")
                .arg(bytesText(bytesOf(status, "filesystem_available_bytes"))));
    if (databaseBytes_ >= 0) {
        journal_->setValueText(
            limitBytes_ > 0 ? tr("%1 of %2").arg(bytesText(databaseBytes_), limitText(limitBytes_))
                            : bytesText(databaseBytes_));
        journal_->setFraction(
            limitBytes_ > 0 ? qMin(1.0, double(databaseBytes_) / double(limitBytes_)) : -1);
    }
}

void SettingsStoragePage::showBreakdown(const QJsonObject& reply) {
    const bool measuring = reply.value("state").toString() == QStringLiteral("measuring");
    measuring_ = measuring;
    if (measuring && !jobsTimer_->isActive())
        jobsTimer_->start();
    const auto breakdown = reply.value("breakdown").toObject();
    if (breakdown.isEmpty())
        return;
    const auto parts = breakdown.value("parts").toObject();
    const auto total = qMax<qint64>(1, bytesOf(breakdown, "database_page_bytes"));
    const QList<qint64> values{bytesOf(parts, "history_bytes"), bytesOf(parts, "image_bytes"),
                               bytesOf(parts, "activity_bytes"), bytesOf(parts, "free_bytes")};
    for (int index = 0; index < parts_.size(); ++index) {
        parts_[index]->setValueText(values[index] >= 0 ? bytesText(values[index]) : tr("Unknown"));
        parts_[index]->setFraction(values[index] >= 0 ? double(values[index]) / double(total) : 0);
    }
    const auto images = bytesOf(parts, "image_count");
    parts_[1]->setCaption(images == 1 ? tr("1 image")
                                      : tr("%1 images").arg(qMax<qint64>(0, images)));
    oldestEvent_ = breakdown.value("oldest_event_at").toString();
    directory_ = breakdown.value("directory").toString();
    QString caption = directory_;
    if (caption.startsWith(QDir::homePath() + QLatin1Char('/')))
        caption = QStringLiteral("~") + caption.mid(QDir::homePath().size());
    // Full detail reaches back to the detail period, or to the oldest row if it is younger.
    auto since = QDateTime::fromString(oldestEvent_, Qt::ISODateWithMs);
    if (detailDays_ > 0) {
        const auto cutoff = QDateTime::currentDateTimeUtc().addDays(-detailDays_);
        if (!since.isValid() || since < cutoff)
            since = cutoff;
    }
    if (since.isValid())
        caption += (caption.isEmpty() ? QString() : QStringLiteral(" · ")) +
                   tr("full detail since %1").arg(day(since));
    journal_->setCaption(caption);
    while (auto* item = largest_->takeAt(0)) {
        if (item->widget())
            item->widget()->deleteLater();
        delete item;
    }
    const auto rows = breakdown.value("largest").toArray();
    for (const auto& value : rows) {
        const auto row = value.toObject();
        QString provider = row.value("provider").toString();
        if (!provider.isEmpty())
            provider[0] = provider[0].toUpper();
        QStringList parts{row.value("title").toString().isEmpty() ? tr("Untitled conversation")
                                                                  : row.value("title").toString()};
        if (!provider.isEmpty())
            parts.append(provider);
        parts.append(bytesText(bytesOf(row, "bytes")));
        if (!row.value("note").toString().isEmpty())
            parts.append(row.value("note").toString());
        auto* line = text(parts.join(QStringLiteral(" · ")), "storageLargestConversation", this);
        const auto number = row.value("number").toString();
        largest_->addWidget(settings_rows::info(
            number.isEmpty() ? tr("Conversation") : QStringLiteral("@agent/") + number, line,
            this));
    }
    largestSection_->setVisible(!rows.isEmpty());
}

void SettingsStoragePage::showCompaction(const QJsonObject& compaction) {
    if (compaction.isEmpty())
        return;
    const auto state = compaction.value("state").toString();
    const bool running =
        state == QStringLiteral("running") || state == QStringLiteral("waiting_for_idle");
    const bool finished = compactionRunning_ && !running;
    compactionRunning_ = running;
    compactionState_ = state;
    const auto last = compaction.value("last").toObject();
    const auto at = QDateTime::fromString(last.value("completed_at").toString(), Qt::ISODateWithMs);
    lastCompaction_->setText(
        at.isValid() ? tr("%1 · %2 → %3")
                           .arg(QLocale().toString(at.toLocalTime(), QLocale::ShortFormat),
                                bytesText(bytesOf(last, "before_bytes")),
                                bytesText(bytesOf(last, "after_bytes")))
                     : tr("Never"));
    compacting_->setVisible(running);
    compact_->setText(running ? tr("Compacting…") : tr("Compact now"));
    if (running) {
        compacting_->setLabel(state == QStringLiteral("waiting_for_idle")
                                  ? tr("Waiting for running turns to finish")
                                  : tr("Compacting"));
        compacting_->setValueText(
            tr("%1 → %2").arg(bytesText(bytesOf(compaction, "before_bytes")),
                              bytesText(bytesOf(compaction, "database_bytes"))));
        compacting_->setFraction(qBound(0.0, compaction.value("sweep_progress").toDouble(), 1.0));
        compacting_->setCaption(tr("Pass %1").arg(compaction.value("sweeps").toInt() + 1));
        if (!jobsTimer_->isActive())
            jobsTimer_->start();
    }
    if (finished) {
        const auto outcome = compaction.value("outcome").toString();
        if (outcome == QStringLiteral("done"))
            jobStatus_->setText(tr("Journal compacted · %1 → %2")
                                    .arg(bytesText(bytesOf(compaction, "before_bytes")),
                                         bytesText(bytesOf(compaction, "database_bytes"))));
        else
            jobStatus_->setText(
                tr("Compaction stopped. %1").arg(compaction.value("error").toString()));
        refresh(); // the sizes, the warning and the parts changed
    }
    setBusy(busy_);
}

void SettingsStoragePage::showExport(const QJsonObject& status) {
    const auto state = status.value("state").toString();
    exportRunning_ = state == QStringLiteral("running");
    if (exportRunning_) {
        jobStatus_->setText(tr("Exporting %1 of %2 conversations…")
                                .arg(status.value("exported").toInt())
                                .arg(status.value("total").toInt()));
        if (!jobsTimer_->isActive())
            jobsTimer_->start();
    } else if (state == QStringLiteral("done")) {
        const int count = status.value("exported").toInt();
        jobStatus_->setText(
            (count == 1 ? tr("Exported 1 conversation to %2")
                        : tr("Exported %1 conversations to %2").arg(count))
                .arg(QDir::toNativeSeparators(status.value("directory").toString())));
    } else if (state == QStringLiteral("failed")) {
        jobStatus_->setText(tr("Export failed. %1").arg(status.value("error").toString()));
    }
    setBusy(busy_);
}

void SettingsStoragePage::startCompaction() {
    if (busy_ || compactionRunning_ || sizeRevision_ < 0)
        return;
    jobStatus_->clear();
    poll("compaction.start", {}, [this](const QJsonObject& reply) {
        compactionRunning_ = true; // the next reply reports how it ended
        showCompaction(reply.value("compaction").toObject());
    });
}

void SettingsStoragePage::startExport() {
    if (busy_ || exportRunning_ || optionsRevision_ < 0)
        return;
    const QString folder =
        chooseFolder_ ? chooseFolder_(this)
                      : QFileDialog::getExistingDirectory(this, tr("Export conversations to"));
    if (folder.isEmpty())
        return;
    jobStatus_->setText(tr("Exporting…"));
    poll("export.start", {{"directory", folder}},
         [this](const QJsonObject& reply) { showExport(reply.value("export").toObject()); });
}
} // namespace ketplus

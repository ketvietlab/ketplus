#include "machine/MachineAttachmentPanel.h"
#include "MachineAttachmentUpload.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QLabel>
#include <QMessageBox>
#include <QMimeData>
#include <QPushButton>
#include <QVBoxLayout>

namespace ketplus {
MachineAttachmentPanel::MachineAttachmentPanel(remote::MachineBackend* backend, QString sessionId,
                                               qint64 generation, QWidget* parent)
    : QWidget(parent), backend_(backend), sessionId_(std::move(sessionId)), generation_(generation),
      layout_(new QVBoxLayout(this)) {
    layout_->setContentsMargins(0, 0, 0, 0);
}
bool MachineAttachmentPanel::allReady() const {
    if (sending_)
        return false;
    for (const auto& row : rows_)
        if (!row.upload || !row.upload->ready())
            return false;
    return true;
}
QJsonArray MachineAttachmentPanel::attachmentIds() const {
    QJsonArray ids;
    if (!allReady())
        return ids;
    for (const auto& row : rows_)
        ids.append(row.upload->attachmentId());
    return ids;
}
void MachineAttachmentPanel::chooseFiles() {
    if (!sending_)
        addFiles(QFileDialog::getOpenFileNames(this, tr("Attach images or text files")));
}
void MachineAttachmentPanel::addFiles(const QStringList& paths) {
    for (const auto& path : paths) {
        QFile file(path);
        QImageReader reader(path);
        const bool image = reader.canRead();
        if (!file.open(QIODevice::ReadOnly) ||
            file.size() > (image ? 20 * 1024 * 1024 : 512 * 1024)) {
            QMessageBox::information(
                this, tr("Cannot attach file"),
                tr("Choose an image up to 20 MB or a text file up to 512 KB."));
            continue;
        }
        QByteArray bytes = file.read(image ? 20 * 1024 * 1024 + 1 : 512 * 1024 + 1);
        QString name = QFileInfo(path).fileName();
        QString type = QStringLiteral("text/plain");
        if (image) {
            const auto format = reader.format().toLower();
            if (format == "png")
                type = QStringLiteral("image/png");
            else if (format == "jpeg" || format == "jpg")
                type = QStringLiteral("image/jpeg");
            else {
                const QImage decoded = reader.read();
                bytes.clear();
                QBuffer buffer(&bytes);
                buffer.open(QIODevice::WriteOnly);
                if (decoded.isNull() || !decoded.save(&buffer, "PNG"))
                    continue;
                name += QStringLiteral(".png");
                type = QStringLiteral("image/png");
            }
        }
        addBytes(name, type, bytes);
    }
}
bool MachineAttachmentPanel::pasteImage() {
    if (sending_ || !QApplication::clipboard()->mimeData()->hasImage())
        return false;
    const QImage image = qvariant_cast<QImage>(QApplication::clipboard()->mimeData()->imageData());
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    if (!image.isNull() && image.save(&buffer, "PNG"))
        addBytes(tr("Clipboard image.png"), QStringLiteral("image/png"), bytes);
    return true;
}
void MachineAttachmentPanel::addBytes(QString name, QString type, QByteArray bytes) {
    if (sending_ || !backend_)
        return;
    if (bytes.isEmpty() || bytes.size() > 20 * 1024 * 1024 ||
        (type == QLatin1String("text/plain") &&
         (bytes.size() > 512 * 1024 || bytes.contains('\0')))) {
        QMessageBox::information(this, tr("Cannot attach file"),
                                 tr("Choose an image up to 20 MB or a text file up to 512 KB."));
        return;
    }
    qint64 total = bytes.size();
    for (const auto& row : rows_)
        total += row.bytes;
    if (rows_.size() >= 5 || total > 40 * 1024 * 1024) {
        QMessageBox::information(this, tr("Cannot attach file"),
                                 tr("Choose up to five files, totalling at most 40 MB."));
        return;
    }
    auto* upload = new remote::MachineAttachmentUpload(backend_, sessionId_, generation_, this);
    auto* row = new QWidget(this);
    auto* line = new QHBoxLayout(row);
    line->setContentsMargins(0, 0, 0, 0);
    auto* label = new QLabel(tr("%1 — Uploading…").arg(name), row);
    label->setWordWrap(true);
    auto* removeButton = new QPushButton(tr("Remove"), row);
    removeButton->setAccessibleName(tr("Remove %1").arg(name));
    line->addWidget(label, 1);
    line->addWidget(removeButton);
    layout_->addWidget(row);
    rows_.append({row, upload, bytes.size()});
    connect(removeButton, &QPushButton::clicked, this, [this, upload] { remove(upload); });
    connect(upload, &remote::MachineAttachmentUpload::progress, label,
            [label, name](qint64 current, qint64 total) {
                label->setText(tr("%1 — Uploading %2%").arg(name).arg(current * 100 / total));
            });
    connect(upload, &remote::MachineAttachmentUpload::finished, this,
            [this, label, name](const QString&, const QString& error) {
                label->setText(error.isEmpty() ? tr("%1 — Ready").arg(name)
                                               : tr("%1 — %2").arg(name, error));
                emit changed();
            });
    emit changed();
    upload->start(std::move(name), std::move(type), std::move(bytes));
}
void MachineAttachmentPanel::remove(remote::MachineAttachmentUpload* upload) {
    if (sending_)
        return;
    for (qsizetype i = 0; i < rows_.size(); ++i) {
        if (rows_[i].upload != upload)
            continue;
        auto row = rows_.takeAt(i);
        // Keep the coordinator alive until a queued begin has completed and cancel is sent.
        disconnect(upload, nullptr, this, nullptr);
        connect(upload, &remote::MachineAttachmentUpload::finished, upload, &QObject::deleteLater);
        upload->cancel();
        if (row.widget)
            row.widget->deleteLater();
        emit changed();
        return;
    }
}
void MachineAttachmentPanel::setSending(bool sending) {
    sending_ = sending;
    setEnabled(!sending);
    emit changed();
}
void MachineAttachmentPanel::sent(bool success) {
    if (success) {
        for (const auto& row : rows_) {
            if (row.widget)
                row.widget->deleteLater();
            if (row.upload)
                row.upload->deleteLater();
        }
        rows_.clear();
    }
    setSending(false);
}
} // namespace ketplus

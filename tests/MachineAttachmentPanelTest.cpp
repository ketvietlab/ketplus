#include "machine/MachineAttachmentPanel.h"
#include "DaemonMachineBackend.h"
#include "MachineAttachmentUpload.h"
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QImage>
#include <QImageReader>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryFile>
#include <QTimer>
#include <QtTest>

using namespace ketplus;
using namespace ketplus::remote;
class MachineAttachmentPanelTest final : public QObject {
    Q_OBJECT
  private slots:
    void invalidFilesUseLocalSubmissionErrorCopy() {
        DaemonMachineBackend backend("machine", MachineBackend::Route::Ssh,
                                     [](const QString&, const QJsonObject&, const QString&, qint64,
                                        MachineBackend::Callback) {});
        MachineAttachmentPanel panel(&backend, "session", 9);
        QSignalSpy rejected(&panel, &MachineAttachmentPanel::inputRejected);
        const auto dismiss = [] {
            QTimer::singleShot(0, [] {
                if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                    box->accept();
            });
        };
        QTemporaryFile file;
        QVERIFY(file.open());
        const QString missing = file.fileName() + ".missing";
        dismiss();
        panel.addFiles({missing});
        QCOMPARE(rejected.last()[0].toString(),
                 QStringLiteral("Cannot attach file (unreadable or over 20 MB): %1").arg(missing));
        QVERIFY(file.resize(21 * 1024 * 1024));
        dismiss();
        panel.addFiles({file.fileName()});
        QCOMPARE(rejected.last()[0].toString(),
                 QStringLiteral("Cannot attach file (unreadable or over 20 MB): %1")
                     .arg(file.fileName()));
        QVERIFY(file.resize(513 * 1024));
        dismiss();
        panel.addFiles({file.fileName()});
        QCOMPARE(
            rejected.last()[0].toString(),
            QStringLiteral("Attach an image or a text file up to 512 KB: %1").arg(file.fileName()));
        QVERIFY(!panel.hasAttachments());
    }

    void removingFailedUploadReleasesItsCoordinator() {
        DaemonMachineBackend backend(
            "machine", MachineBackend::Route::Ssh,
            [](const QString&, const QJsonObject&, const QString&, qint64,
               MachineBackend::Callback callback) { callback({}, "Staging failed"); });
        MachineAttachmentPanel panel(&backend, "session", 9);
        QTemporaryFile file;
        QVERIFY(file.open());
        file.write("text");
        file.flush();
        panel.addFiles({file.fileName()});
        QCOMPARE(panel.findChildren<MachineAttachmentUpload*>().size(), 1);
        auto* remove = panel.findChild<QPushButton*>();
        QVERIFY(remove);
        remove->click();
        QTRY_VERIFY(panel.findChildren<MachineAttachmentUpload*>().isEmpty());
        QVERIFY(panel.discardable());
    }

    void decodeFailureIsReportedAndNullClipboardIsNotConsumed() {
        DaemonMachineBackend backend("machine", MachineBackend::Route::Ssh,
                                     [](const QString&, const QJsonObject&, const QString&, qint64,
                                        MachineBackend::Callback) {});
        MachineAttachmentPanel panel(&backend, "session", 9);
        QSignalSpy rejected(&panel, &MachineAttachmentPanel::inputRejected);
        QImage image(8, 8, QImage::Format_RGB32);
        image.fill(Qt::blue);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        QVERIFY(image.save(&buffer, "BMP"));
        QTemporaryFile file;
        QVERIFY(file.open());
        file.write(bytes.left(54));
        file.flush();
        QImageReader reader(file.fileName());
        QVERIFY(reader.canRead());
        QTimer::singleShot(0, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                box->accept();
        });
        panel.addFiles({file.fileName()});
        QCOMPARE(rejected.size(), 1);
        QVERIFY(rejected[0][0].toString().startsWith("Could not decode the attachment image:"));
        QVERIFY(!panel.hasAttachments());
        QApplication::clipboard()->setImage(QImage());
        QTimer::singleShot(0, [] {
            if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget()))
                box->accept();
        });
        QVERIFY(!panel.pasteImage());
        QVERIFY(!panel.hasAttachments());
        QApplication::clipboard()->clear();
    }

    void fileStagesBeforeSendAndFailedSendKeepsDraft() {
        QStringList kinds;
        QByteArray received;
        DaemonMachineBackend backend(
            "machine", MachineBackend::Route::Ssh,
            [&](const QString& kind, const QJsonObject& payload, const QString& session,
                qint64 generation, MachineBackend::Callback callback) {
                QCOMPARE(session, QStringLiteral("session"));
                QCOMPARE(generation, 9);
                kinds.append(kind);
                if (kind.endsWith("begin"))
                    callback({{"revision", 1}}, {});
                else if (kind.endsWith("chunk")) {
                    received += QByteArray::fromBase64(payload.value("data").toString().toLatin1(),
                                                       QByteArray::Base64UrlEncoding);
                    callback({{"accepted", true}}, {});
                } else
                    callback({{"status", "ready"}}, {});
            });
        QTemporaryFile file;
        QVERIFY(file.open());
        const QByteArray content("Vietnamese: Tiếng Việt\nsecond line");
        QCOMPARE(file.write(content), content.size());
        file.flush();
        MachineAttachmentPanel panel(&backend, "session", 9);
        panel.addFiles({file.fileName()});
        QVERIFY(panel.hasAttachments());
        QVERIFY(!panel.allReady());
        QTRY_VERIFY(panel.allReady());
        QCOMPARE(received, content);
        QCOMPARE(kinds, QStringList({"attachment.stage.begin", "attachment.stage.chunk",
                                     "attachment.stage.commit"}));
        QCOMPARE(panel.attachmentIds().size(), 1);
        panel.setSending(true);
        QVERIFY(!panel.allReady());
        panel.sent(false);
        QVERIFY(panel.allReady());
        QCOMPARE(panel.attachmentIds().size(), 1);
        panel.sent(true);
        QVERIFY(!panel.hasAttachments());
    }
    void clipboardImageUsesSameBoundedStagingPath() {
        QByteArray received;
        DaemonMachineBackend backend(
            "machine", MachineBackend::Route::Ssh,
            [&](const QString& kind, const QJsonObject& payload, const QString&, qint64,
                MachineBackend::Callback callback) {
                if (kind.endsWith("begin")) {
                    QCOMPARE(payload.value("media_type").toString(), QStringLiteral("image/png"));
                    callback({{"revision", 1}}, {});
                } else if (kind.endsWith("chunk")) {
                    received += QByteArray::fromBase64(payload.value("data").toString().toLatin1(),
                                                       QByteArray::Base64UrlEncoding);
                    callback({{"accepted", true}}, {});
                } else
                    callback({{"status", "ready"}}, {});
            });
        QImage image(8, 8, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        QApplication::clipboard()->setImage(image);
        MachineAttachmentPanel panel(&backend, "session", 9);
        QVERIFY(panel.pasteImage());
        QTRY_VERIFY(panel.allReady());
        QCOMPARE(QImage::fromData(received).size(), image.size());
        QApplication::clipboard()->clear();
    }
};
QTEST_MAIN(MachineAttachmentPanelTest)
#include "MachineAttachmentPanelTest.moc"

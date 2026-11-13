#include "machine/MachineAttachmentPanel.h"
#include "DaemonMachineBackend.h"
#include <QApplication>
#include <QClipboard>
#include <QImage>
#include <QTemporaryFile>
#include <QtTest>

using namespace ketplus;
using namespace ketplus::remote;
class MachineAttachmentPanelTest final : public QObject {
    Q_OBJECT
  private slots:
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

#include "storage/McpMediaStore.h"

#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

using namespace ketplus::daemon;

namespace {
QByteArray picture(QRgb color) {
    QImage image(160, 96, QImage::Format_RGB32);
    image.fill(color);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::WriteOnly) || !image.save(&buffer, "PNG"))
        return {};
    return bytes;
}

bool put(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray read(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
} // namespace

class McpMediaPreviewTest final : public QObject {
    Q_OBJECT

  private slots:
    void previewRequiresKnownOwnershipAndMatchingBytes_data() {
        QTest::addColumn<QString>("fault");
        for (const auto* value :
             {"valid", "malformed", "oversized", "file-symlink", "root-symlink",
              "media-root-symlink", "wrong-blob", "reference-newline", "unreadable", "missing",
              "refs-directory", "blob-symlink", "blob-oversized"})
            QTest::newRow(value) << QString(value);
    }

    void previewRequiresKnownOwnershipAndMatchingBytes() {
        QFETCH(QString, fault);
#ifdef Q_OS_WIN
        if (fault == "unreadable")
            QSKIP("This permission-denial fixture requires POSIX file modes.");
#endif
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto media = directory.filePath("media");
        McpMediaStore store(media);
        const auto png = picture(qRgb(255, 0, 0));
        QVERIFY(!png.isEmpty());
        const auto stored = store.storeImage(png, "image/png", "session");
        const auto reference = stored.value("media_ref").toString();
        QVERIFY(!reference.isEmpty());
        QVERIFY(store.preview("session", reference, 64).value("ok").toBool());
        const auto hash = reference.mid(7).toLatin1();
        const auto blob = media + '/' + hash + ".png";
        const auto refs = media + "/refs";
        const auto owner = refs + '/' +
                           QCryptographicHash::hash("session", QCryptographicHash::Sha256).toHex() +
                           ".refs";
        const auto original = hash + '\n';
        const auto backup = directory.filePath("backup");
        const auto foreign = directory.filePath("foreign");
        QByteArray changed;
        if (fault == "malformed" || fault == "oversized") {
            changed = fault == "malformed"
                          ? original + "unsupported\n"
                          : original.repeated(4 * 1024 * 1024 / original.size() + 1);
            QVERIFY(put(owner, changed));
        } else if (fault == "file-symlink") {
            QVERIFY(put(foreign, original));
            QVERIFY(QFile::remove(owner));
            QVERIFY(QFile::link(foreign, owner));
        } else if (fault == "root-symlink" || fault == "media-root-symlink") {
            const auto root = fault == "root-symlink" ? refs : media;
            QVERIFY(QDir().rename(root, backup));
            QVERIFY(QFile::link(backup, root));
        } else if (fault == "wrong-blob") {
            changed = picture(qRgb(0, 0, 255));
            QVERIFY(!changed.isEmpty());
            QVERIFY(changed != png);
            QVERIFY(put(blob, changed));
        } else if (fault == "unreadable") {
            QVERIFY(QFile::setPermissions(owner, QFileDevice::WriteOwner));
            QFile denied(owner);
            QVERIFY(!denied.open(QIODevice::ReadOnly));
        } else if (fault == "missing" || fault == "refs-directory") {
            QVERIFY(QFile::remove(owner));
            if (fault == "refs-directory")
                QVERIFY(QDir().mkpath(owner));
        } else if (fault == "blob-symlink") {
            QVERIFY(put(foreign, png));
            QVERIFY(QFile::remove(blob));
            QVERIFY(QFile::link(foreign, blob));
        } else if (fault == "blob-oversized") {
            QFile file(blob);
            QVERIFY(file.open(QIODevice::ReadWrite));
            QVERIFY(file.resize(McpMediaStore::maximumImageBytes + 1));
        }
        const auto result =
            store.preview("session", reference + (fault == "reference-newline" ? "\n" : ""), 64);
        QCOMPARE(result.value("ok").toBool(), fault == "valid");
        if (fault != "valid") {
            QCOMPARE(result.value("error_code").toString(), "media_unavailable");
            QVERIFY(!result.contains("data"));
            QVERIFY(!result.contains("media_ref"));
        }
        QVERIFY(QFileInfo::exists(blob));
        if (fault == "malformed" || fault == "oversized")
            QCOMPARE(read(owner), changed);
        if (fault == "wrong-blob")
            QCOMPARE(read(blob), changed);
        if (fault == "file-symlink") {
            QCOMPARE(read(foreign), original);
            QVERIFY(QFile::remove(owner));
        } else if (fault == "root-symlink" || fault == "media-root-symlink") {
            const auto root = fault == "root-symlink" ? refs : media;
            QVERIFY(QFile::remove(root));
            QVERIFY(QDir().rename(backup, root));
        } else if (fault == "unreadable") {
            QVERIFY(QFile::setPermissions(owner, QFileDevice::ReadOwner | QFileDevice::WriteOwner));
        } else if (fault == "refs-directory") {
            QVERIFY(QDir().rmdir(owner));
        } else if (fault == "blob-symlink") {
            QCOMPARE(read(foreign), png);
            QVERIFY(QFile::remove(blob));
        }
        QVERIFY(put(owner, original));
        QVERIFY(put(blob, png));
        const auto repaired = store.preview("session", reference, 64);
        QVERIFY(repaired.value("ok").toBool());
        QCOMPARE(repaired.value("media_ref").toString(), reference);
        QCOMPARE(repaired.value("mime").toString(), "image/jpeg");
        const auto jpeg = QByteArray::fromBase64(repaired.value("data").toString().toLatin1(),
                                                 QByteArray::Base64UrlEncoding);
        const auto decoded = QImage::fromData(jpeg, "JPEG");
        QVERIFY(!decoded.isNull());
        QVERIFY(decoded.width() <= 64 && decoded.height() <= 64);
        QCOMPARE(read(owner), original);
        QCOMPARE(read(blob), png);
    }
};

QTEST_GUILESS_MAIN(McpMediaPreviewTest)
#include "McpMediaPreviewTest.moc"

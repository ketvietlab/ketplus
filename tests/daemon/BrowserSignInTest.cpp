#include "cloud/BrowserSignIn.h"
#include "cloud/ControlEndpoint.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDeadlineTimer>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QtTest>

using namespace ketplus::daemon;

namespace {

// Sends one request to the sign-in listener and returns the status line of its answer.
QByteArray get(const QUrl& redirect, const QByteArray& target, const QByteArray& method = "GET") {
    QTcpSocket socket;
    socket.connectToHost(redirect.host(), static_cast<quint16>(redirect.port()));
    socket.write(method + ' ' + target + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n");
    // The listener lives on this thread, so the event loop must turn while waiting.
    QDeadlineTimer deadline(5000);
    QByteArray response;
    while (!deadline.hasExpired() && !response.contains("\r\n")) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        response += socket.readAll();
    }
    return response.left(response.indexOf("\r\n"));
}

} // namespace

class BrowserSignInTest final : public QObject {
    Q_OBJECT

  private slots:
    void controlEndpointAppendsToTheBasePath() {
        QCOMPARE(controlEndpoint(QUrl(QStringLiteral("https://app.example/native/ketplus")),
                                 QStringLiteral("/v2/auth/handoffs")),
                 QUrl(QStringLiteral("https://app.example/native/ketplus/v2/auth/handoffs")));
        QCOMPARE(controlEndpoint(QUrl(QStringLiteral("https://app.example/native/ketplus/?x=1#f")),
                                 QStringLiteral("/v2/relay")),
                 QUrl(QStringLiteral("https://app.example/native/ketplus/v2/relay")));
        QCOMPARE(controlEndpoint(QUrl(QStringLiteral("http://127.0.0.1:43110")),
                                 QStringLiteral("/v2/machines:enroll?limit=5")),
                 QUrl(QStringLiteral("http://127.0.0.1:43110/v2/machines:enroll?limit=5")));
    }

    void challengeIsTheVerifierDigest() {
        BrowserSignIn signIn;
        QString error;
        QVERIFY2(signIn.listen(&error), qPrintable(error));
        const QUrl redirect(signIn.redirectUri());
        QCOMPARE(redirect.scheme(), QStringLiteral("http"));
        QCOMPARE(redirect.host(), QStringLiteral("127.0.0.1"));
        QVERIFY(redirect.port() >= 1024);
        QCOMPARE(redirect.path(), QStringLiteral("/callback"));
        QCOMPARE(signIn.codeVerifier().size(), 43);
        QVERIFY(signIn.state().size() >= 16);
        QVERIFY(signIn.state() != signIn.codeVerifier());
        const QByteArray digest =
            QCryptographicHash::hash(signIn.codeVerifier().toLatin1(), QCryptographicHash::Sha256)
                .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
        QCOMPARE(signIn.codeChallenge(), QString::fromLatin1(digest));
    }

    void onlyTheMatchingCallbackFinishes() {
        BrowserSignIn signIn;
        QString error;
        QVERIFY2(signIn.listen(&error), qPrintable(error));
        QSignalSpy received(&signIn, &BrowserSignIn::codeReceived);
        QSignalSpy failed(&signIn, &BrowserSignIn::failed);
        const QUrl redirect(signIn.redirectUri());
        const QByteArray code = "code_0123456789abcdef";
        const QByteArray state = signIn.state().toLatin1();

        QCOMPARE(get(redirect, "/favicon.ico"), QByteArray("HTTP/1.1 404 Not Found"));
        QCOMPARE(get(redirect, "/callback?code=" + code + "&state=" + state, "POST"),
                 QByteArray("HTTP/1.1 404 Not Found"));
        QCOMPARE(get(redirect, "/callback?code=" + code + "&state=wrong_state_0123456789"),
                 QByteArray("HTTP/1.1 400 Bad Request"));
        QCOMPARE(get(redirect, "/callback?code=short&state=" + state),
                 QByteArray("HTTP/1.1 400 Bad Request"));
        QCOMPARE(received.count(), 0);

        QCOMPARE(get(redirect, "/callback?code=" + code + "&state=" + state),
                 QByteArray("HTTP/1.1 200 OK"));
        QCOMPARE(received.count(), 1);
        QCOMPARE(received.at(0).at(0).toString(), QString::fromLatin1(code));
        QCOMPARE(failed.count(), 0);
        // The code is redeemed after the listener closes, with the redirect it was issued for.
        QCOMPARE(QUrl(signIn.redirectUri()), redirect);

        // The listener closes with the first valid callback; nothing can replay it.
        QTcpSocket late;
        late.connectToHost(redirect.host(), static_cast<quint16>(redirect.port()));
        QVERIFY(!late.waitForConnected(1000));
        QCOMPARE(received.count(), 1);
    }

    void aDeclinedSignInEndsAtOnceWithAFixedMessage_data() {
        QTest::addColumn<QByteArray>("error");
        QTest::addColumn<QString>("message");
        QTest::newRow("declined") << QByteArray("access_denied")
                                  << QStringLiteral("Sign-in was declined.");
        QTest::newRow("other") << QByteArray("server_error")
                               << QStringLiteral("Sign-in did not finish. Try again.");
    }

    void aDeclinedSignInEndsAtOnceWithAFixedMessage() {
        QFETCH(QByteArray, error);
        QFETCH(QString, message);
        BrowserSignIn signIn;
        QString listenError;
        QVERIFY2(signIn.listen(&listenError), qPrintable(listenError));
        QSignalSpy received(&signIn, &BrowserSignIn::codeReceived);
        QSignalSpy failed(&signIn, &BrowserSignIn::failed);
        const QUrl redirect(signIn.redirectUri());
        const QByteArray state = signIn.state().toLatin1();
        // Another page's error cannot end this sign-in.
        QCOMPARE(get(redirect, "/callback?error=" + error + "&state=wrong_state_0123456789"),
                 QByteArray("HTTP/1.1 400 Bad Request"));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(get(redirect, "/callback?error=" + error +
                                   "&error_description=Visit%20evil.example&state=" + state),
                 QByteArray("HTTP/1.1 200 OK"));
        QCOMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), message);
        QCOMPARE(received.count(), 0);
        QTcpSocket late;
        late.connectToHost(redirect.host(), static_cast<quint16>(redirect.port()));
        QVERIFY(!late.waitForConnected(1000));
    }
};

QTEST_GUILESS_MAIN(BrowserSignInTest)
#include "BrowserSignInTest.moc"

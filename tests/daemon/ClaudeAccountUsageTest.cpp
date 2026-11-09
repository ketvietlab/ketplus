#include "providers/ClaudeAccountUsage.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QTimer>
#include <QUrlQuery>
#include <QtTest>
using namespace ketplus::daemon;
namespace {
class Response : public QNetworkReply {
  public:
    QByteArray body;
    Response(QByteArray bytes, int code, QObject* parent) : QNetworkReply(parent), body(bytes) {
        setAttribute(QNetworkRequest::HttpStatusCodeAttribute, code);
        open(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            setFinished(true);
            emit finished();
        });
    }
    void abort() override { setError(OperationCanceledError, "aborted"); }
    qint64 readData(char* target, qint64 size) override {
        const auto count = qMin(size, body.size());
        if (!count)
            return -1;
        memcpy(target, body.constData(), count);
        body.remove(0, count);
        return count;
    }
};
class Network : public QNetworkAccessManager {
  public:
    QList<QJsonObject> bodies;
    QList<QNetworkRequest> requests;
    int code{200};
    QNetworkReply* createRequest(Operation op, const QNetworkRequest& request,
                                 QIODevice*) override {
        Q_ASSERT(op == GetOperation);
        requests.append(request);
        const auto body = bodies.isEmpty() ? QJsonObject{} : bodies.takeFirst();
        return new Response(QJsonDocument(body).toJson(), code, this);
    }
};
QJsonObject page(const QJsonObject& metric, const QString& kind, bool more = false) {
    return {{"data", QJsonArray{QJsonObject{{kind, metric}}}},
            {"has_more", more},
            {"next_page", more ? "next" : ""},
            {"data_refreshed_at", "2026-10-31T10:00:00Z"}};
}
} // namespace
class ClaudeAccountUsageTest : public QObject {
    Q_OBJECT
  private slots:
    void reportsAreScopedValidatedCachedAndRevoked() {
        QByteArray stored;
        ClaudeCredentials::Storage storage{[&](QString*) { return stored; },
                                           [&](const QByteArray& b, QString*) {
                                               stored = b;
                                               return true;
                                           },
                                           "memory"};
        Network network;
        ClaudeAccountUsage usage(storage, nullptr, &network);
        QJsonObject result;
        QString error;
        int calls = 0;
        const auto reply = [&](const QJsonObject& r, const QString& e) {
            result = r;
            error = e;
            ++calls;
        };
        usage.handle("save", {{"key", "sk-ant-inference"}}, reply);
        QVERIFY(!error.isEmpty());
        QVERIFY(stored.isEmpty());
        usage.handle("save", {{"key", "sk-ant-admin01-fake"}}, reply);
        QVERIFY(error.isEmpty());
        QVERIFY(result.value("configured").toBool());
        QVERIFY(!QJsonDocument(result).toJson().contains("sk-ant"));
        network.bodies = {page({{"amount", "125.000000"}, {"currency", "USD"}}, "cost", true),
                          page({{"amount", "75.125000"}, {"currency", "USD"}}, "cost"),
                          page({{"total_tokens", 1200}}, "usage")};
        const int before = calls;
        usage.handle("refresh", {}, reply);
        QTRY_COMPARE(calls, before + 1);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QCOMPARE(result.value("report").toObject().value("costUsd").toDouble(), 2.00125);
        QCOMPARE(result.value("report").toObject().value("totalTokens").toInt(), 1200);
        QCOMPARE(network.requests.size(), 3);
        for (const auto& request : network.requests) {
            QCOMPARE(request.url().host(), "api.anthropic.com");
            QCOMPARE(request.rawHeader("x-api-key"), "sk-ant-admin01-fake");
            QCOMPARE(request.attribute(QNetworkRequest::RedirectPolicyAttribute).toInt(),
                     int(QNetworkRequest::ManualRedirectPolicy));
            QVERIFY(!request.url().toString().contains("sk-ant"));
            const QUrlQuery query(request.url());
            QVERIFY(query.queryItemValue("starting_at").contains("-01T00:00:00"));
        }
        QCOMPARE(QUrlQuery(network.requests.at(1).url()).queryItemValue("page"), "next");
        usage.handle("refresh", {}, reply);
        QCOMPARE(network.requests.size(), 3);
        usage.handle("remove", {}, reply);
        QVERIFY(!result.value("configured").toBool());
        QVERIFY(result.value("report").toObject().isEmpty());
        QVERIFY(!stored.contains("sk-ant"));
        usage.handle("refresh", {}, reply);
        QCOMPARE(network.requests.size(), 3);
    }
    void removeClearsAnUnreadableAdminEntry() {
        QByteArray stored = "{broken";
        ClaudeCredentials::Storage storage{[&](QString*) { return stored; },
                                           [&](const QByteArray& b, QString*) {
                                               stored = b;
                                               return true;
                                           },
                                           "memory"};
        Network network;
        ClaudeAccountUsage usage(storage, nullptr, &network);
        QJsonObject result;
        QString error;
        const auto reply = [&](const QJsonObject& r, const QString& e) {
            result = r;
            error = e;
        };
        usage.handle("status", {}, reply);
        QVERIFY(!error.isEmpty());
        usage.handle("remove", {}, reply);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(!result.value("configured").toBool());
        usage.handle("status", {}, reply);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY(network.requests.isEmpty());
    }
    void malformedPartialAndRotatedReportsNeverBecomeTotals() {
        QByteArray stored;
        ClaudeCredentials::Storage storage{[&](QString*) { return stored; },
                                           [&](const QByteArray& b, QString*) {
                                               stored = b;
                                               return true;
                                           },
                                           "memory"};
        Network network;
        ClaudeAccountUsage usage(storage, nullptr, &network);
        QJsonObject result;
        QString error;
        int calls = 0;
        const auto reply = [&](const QJsonObject& r, const QString& e) {
            result = r;
            error = e;
            ++calls;
        };
        usage.handle("save", {{"key", "sk-ant-admin01-first"}}, reply);
        network.bodies = {page({{"amount", "123"}, {"currency", "USD"}}, "cost"),
                          page({{"total_tokens", -1}}, "usage")};
        int before = calls;
        usage.handle("refresh", {}, reply);
        QTRY_COMPARE(calls, before + 1);
        QVERIFY(!error.isEmpty());
        usage.handle("status", {}, reply);
        QVERIFY(result.value("report").toObject().isEmpty());
        network.bodies = {page({{"amount", "123"}, {"currency", "USD"}}, "cost")};
        usage.handle("refresh", {}, reply);
        usage.handle("save", {{"key", "sk-ant-admin01-second"}}, reply);
        before = calls;
        QTRY_COMPARE(calls, before + 1);
        QVERIFY(error.contains("changed"));
        usage.handle("status", {}, reply);
        QVERIFY(result.value("report").toObject().isEmpty());
        network.code = 403;
        before = calls;
        usage.handle("refresh", {}, reply);
        QTRY_COMPARE(calls, before + 1);
        QVERIFY(error.contains("cannot read"));
        QVERIFY(!error.contains("sk-ant"));
    }
};
QTEST_GUILESS_MAIN(ClaudeAccountUsageTest)
#include "ClaudeAccountUsageTest.moc"

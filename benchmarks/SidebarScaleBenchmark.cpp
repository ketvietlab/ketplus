// The product sidebar with a long-lived install's worth of conversations. Offscreen,
// synthetic rows only, preferences in a temporary INI store; no daemon, no provider.
//
// Usage: ketplus_sidebar_scale_benchmark [conversations=3000] [repositories=150] [rounds=5]
// Prints one SIDEBAR_* line per measured update, the median of `rounds` runs, in ms.

#include "KetAppearance.h"
#include "KetStyle.h"
#include "workspace/SidebarRepositoryList.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QSettings>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <functional>

using ketplus::SidebarRepository;

namespace {

QVector<SidebarRepository> workspace(const int conversations, const int repositories) {
    QVector<SidebarRepository> result;
    for (int index = 0; index < repositories; ++index) {
        const QString path =
            QStringLiteral("/synthetic/repo-%1").arg(index, 3, 10, QLatin1Char('0'));
        result.append({.path = path,
                       .name = QStringLiteral("repo-%1").arg(index, 3, 10, QLatin1Char('0')),
                       .pinnable = true});
    }
    quint32 seed = 20261117;
    for (int index = 0; index < conversations; ++index) {
        seed = seed * 1664525u + 1013904223u;
        // A few busy repositories hold most conversations, as real installs do.
        const int a = int((seed >> 8) % 1000);
        seed = seed * 1664525u + 1013904223u;
        const int b = int((seed >> 8) % 1000);
        SidebarRepository& repository = result[(a * b / 1000) % repositories];
        repository.conversations.append(QVariantMap{
            {QStringLiteral("id"), QStringLiteral("c%1").arg(index)},
            {QStringLiteral("path"), repository.path},
            {QStringLiteral("executionPath"), repository.path},
            {QStringLiteral("provider"),
             index % 3 == 0 ? QStringLiteral("claude") : QStringLiteral("codex")},
            {QStringLiteral("title"), QStringLiteral("Synthetic conversation %1").arg(index)},
            {QStringLiteral("number"), index + 1},
            {QStringLiteral("started"), true},
            {QStringLiteral("running"), false},
            {QStringLiteral("seen"), true},
            {QStringLiteral("selected"), false},
            {QStringLiteral("lastTurnStartedAt"), qint64(1'700'000'000'000) + index * 5000}});
    }
    return result;
}

double median(QVector<double> samples) {
    std::sort(samples.begin(), samples.end());
    return samples.at(samples.size() / 2);
}

} // namespace

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    ket::Style::install(app);

    const QStringList args = app.arguments();
    const int conversations = args.size() > 1 ? args.at(1).toInt() : 3000;
    const int repositories = std::max(1, args.size() > 2 ? args.at(2).toInt() : 150);
    const int rounds = std::max(1, args.size() > 3 ? args.at(3).toInt() : 5);
    const QVector<SidebarRepository> base = workspace(conversations, repositories);

    QHash<QString, QVector<double>> results;
    for (int round = 0; round < rounds; ++round) {
        QScrollArea area;
        area.setWidgetResizable(true);
        auto* list = new ketplus::SidebarRepositoryList;
        area.setWidget(list);
        area.resize(300, 900);
        area.show();
        const auto measure = [&](const char* name, const QVector<SidebarRepository>& next) {
            QElapsedTimer timer;
            timer.start();
            list->setRepositories(next);
            // Layout and paint are part of what the user waits for.
            QCoreApplication::sendPostedEvents();
            area.grab();
            results[QString::fromLatin1(name)].append(double(timer.nsecsElapsed()) / 1e6);
        };
        measure("SIDEBAR_INITIAL_MS", base);
        measure("SIDEBAR_UNCHANGED_MS", base);

        QVector<SidebarRepository> selected = base;
        selected[0].active = true;
        QVariantMap first = selected[0].conversations.first().toMap();
        first.insert(QStringLiteral("selected"), true);
        selected[0].conversations[0] = first;
        measure("SIDEBAR_SELECT_MS", selected);

        QVector<SidebarRepository> running = selected;
        first.insert(QStringLiteral("running"), true);
        running[0].conversations[0] = first;
        measure("SIDEBAR_RUNNING_MS", running);

        QVector<SidebarRepository> renamed = running;
        first.insert(QStringLiteral("title"), QStringLiteral("Renamed while it runs"));
        renamed[0].conversations[0] = first;
        measure("SIDEBAR_RENAME_MS", renamed);

        QVector<SidebarRepository> added = renamed;
        QVariantMap created = first;
        created.insert(QStringLiteral("id"), QStringLiteral("new"));
        created.insert(QStringLiteral("title"), QStringLiteral("New conversation"));
        created.insert(QStringLiteral("lastTurnStartedAt"), qint64(1'900'000'000'000));
        added[0].conversations.append(created);
        measure("SIDEBAR_ADD_MS", added);
    }
    std::printf("SIDEBAR_CONVERSATIONS=%d\nSIDEBAR_REPOSITORIES=%d\n", conversations, repositories);
    for (const char* name : {"SIDEBAR_INITIAL_MS", "SIDEBAR_UNCHANGED_MS", "SIDEBAR_SELECT_MS",
                             "SIDEBAR_RUNNING_MS", "SIDEBAR_RENAME_MS", "SIDEBAR_ADD_MS"})
        std::printf("%s=%.2f\n", name, median(results.value(QString::fromLatin1(name))));
    return 0;
}

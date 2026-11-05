#include "app/RepositoryMetadataStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QTemporaryDir>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid())
        return 1;
    const auto root = directory.filePath("repo");
    QProcess git;
    git.start("git", {"init", "-q", root});
    if (!git.waitForFinished(5000) || git.exitCode() != 0)
        return 1;
    ketplus::RepositoryMetadataStore store(directory.filePath("cache.sqlite"));
    store.save({root, root + "/.git", root});
    if (!store.lookup(root))
        return 1;
    constexpr int count = 100;
    QElapsedTimer timer;
    timer.start();
    for (int index = 0; index < count; ++index)
        if (!store.lookup(root))
            return 1;
    const double cacheMs = timer.nsecsElapsed() / 1e6;
    timer.restart();
    for (int index = 0; index < count; ++index) {
        git.start("git", {"-C", root, "rev-parse", "--path-format=absolute", "--git-common-dir",
                          "--show-toplevel"});
        if (!git.waitForFinished(5000) || git.exitCode() != 0)
            return 1;
    }
    std::printf("LOOKUPS=%d\nCACHE_READ_TOTAL_MS=%.3f\nGIT_LOOKUP_TOTAL_MS=%.3f\n", count, cacheMs,
                timer.nsecsElapsed() / 1e6);
}

#include "BundledBrowserRuntime.h"
#include "BrowserToolLaunch.h"
#include "BundledAgentSkills.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>

using namespace ketplus::browser_tools;

namespace {

void writeFile(const QString& path, const QByteArray& bytes = {},
               const QFileDevice::Permissions permissions = {}) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(bytes);
    file.close();
    if (permissions)
        QVERIFY(file.setPermissions(permissions));
}

QJsonObject validManifest() {
    return QJsonObject{
        {QStringLiteral("name"), QStringLiteral("ketplus-browser")},
        {QStringLiteral("version"), QStringLiteral("0.1.0")},
        {QStringLiteral("nodeVersion"), QStringLiteral("v24.21.0")},
        {QStringLiteral("node"), QStringLiteral("bin/node")},
        {QStringLiteral("protocols"), QJsonArray{QStringLiteral("browser.v0")}},
        {QStringLiteral("entrypoints"),
         QJsonObject{{QStringLiteral("mcp"), QStringLiteral("src/mcp.mjs")},
                     {QStringLiteral("jsonrpc"), QStringLiteral("src/cli.mjs")}}},
        {QStringLiteral("skill"), QStringLiteral(".claude/skills/ketplus-browser/SKILL.md")},
    };
}

// A fake KetPlus.app/Contents with MacOS/, Helpers/ketplus_daemon's dir and the runtime.
struct FakeBundle final {
    QTemporaryDir root{QDir::tempPath() + QStringLiteral("/KetPlus bundle-XXXXXX")};
    QString contents() const { return root.filePath(QStringLiteral("KetPlus.app/Contents")); }
    QString macos() const { return contents() + QStringLiteral("/MacOS"); }
    QString helpers() const { return contents() + QStringLiteral("/Helpers"); }
    QString resources() const { return contents() + QStringLiteral("/Resources"); }
    QString runtimeParent{};
    QString runtime() const {
        return (runtimeParent.isEmpty() ? resources() : runtimeParent) +
               QStringLiteral("/ketplus-browser");
    }

    void create(const QJsonObject& manifest = validManifest(), const bool executableNode = true) {
        QDir().mkpath(macos());
        writeFile(runtime() + QStringLiteral("/bin/node"), "#!/bin/sh\n",
                  executableNode ? QFileDevice::ReadOwner | QFileDevice::ExeOwner
                                 : QFileDevice::ReadOwner | QFileDevice::WriteOwner);
        writeFile(runtime() + QStringLiteral("/src/mcp.mjs"));
        writeFile(runtime() + QStringLiteral("/src/cli.mjs"));
        writeFile(runtime() + QStringLiteral("/.claude/skills/ketplus-browser/SKILL.md"));
        writeManifest(manifest);
    }
    void writeManifest(const QJsonObject& manifest) const {
        writeFile(runtime() + QStringLiteral("/runtime.json"), QJsonDocument(manifest).toJson());
    }
};

} // namespace

class BundledBrowserRuntimeTest final : public QObject {
    Q_OBJECT

  private slots:
    void findsRuntimeFromDesktopAndDaemonDirectories() {
        FakeBundle bundle;
        bundle.create();
        const QString canonicalRoot = QFileInfo(bundle.runtime()).canonicalFilePath();

        for (const QString& appDir : {bundle.macos(), bundle.helpers()}) {
            const auto runtime = findBundledBrowserRuntime(appDir);
            QVERIFY2(runtime.has_value(), qPrintable(appDir));
            QCOMPARE(runtime->rootDir, canonicalRoot);
            QCOMPARE(runtime->nodePath, canonicalRoot + QStringLiteral("/bin/node"));
            QCOMPARE(runtime->mcpEntry, canonicalRoot + QStringLiteral("/src/mcp.mjs"));
            QCOMPARE(runtime->jsonRpcEntry, canonicalRoot + QStringLiteral("/src/cli.mjs"));
            QCOMPARE(runtime->skillFile,
                     canonicalRoot + QStringLiteral("/.claude/skills/ketplus-browser/SKILL.md"));
            QCOMPARE(runtime->version, QStringLiteral("0.1.0"));
            QCOMPARE(runtime->nodeVersion, QStringLiteral("v24.21.0"));
        }
        QVERIFY(!findBundledBrowserRuntime(bundle.root.path()).has_value());

        // Also found in Contents/Helpers/ketplus-browser, and as the daemon's sibling.
        for (const QString& parent : {QStringLiteral("Helpers"), QStringLiteral("MacOS")}) {
            FakeBundle other;
            other.runtimeParent = other.contents() + QLatin1Char('/') + parent;
            other.create();
            const auto runtime = findBundledBrowserRuntime(other.runtimeParent);
            QVERIFY2(runtime.has_value(), qPrintable(parent));
            QCOMPARE(runtime->rootDir, QFileInfo(other.runtime()).canonicalFilePath());
        }
        QCOMPARE(bundledBrowserRuntimeCandidates(QStringLiteral("/A/Contents/Helpers")),
                 (QStringList{QStringLiteral("/A/Contents/Resources/ketplus-browser"),
                              QStringLiteral("/A/Contents/Helpers/ketplus-browser")}));
        QVERIFY(!findBundledBrowserRuntime(QString()).has_value());
    }

    void rejectsInvalidRuntimes() {
        {
            FakeBundle bundle;
            bundle.create(validManifest(), false);
            QVERIFY(
                !loadBundledBrowserRuntime(bundle.runtime()).has_value()); // node not executable
        }
        const auto rejects = [](const std::function<void(QJsonObject&)>& mutate) {
            FakeBundle bundle;
            bundle.create();
            QJsonObject manifest = validManifest();
            mutate(manifest);
            bundle.writeManifest(manifest);
            return !loadBundledBrowserRuntime(bundle.runtime()).has_value();
        };
        QVERIFY(
            rejects([](QJsonObject& m) { m[QStringLiteral("name")] = QStringLiteral("other"); }));
        QVERIFY(rejects([](QJsonObject& m) {
            m[QStringLiteral("protocols")] = QJsonArray{QStringLiteral("browser.v1")};
        }));
        QVERIFY(rejects([](QJsonObject& m) { m.remove(QStringLiteral("version")); }));
        QVERIFY(rejects([](QJsonObject& m) {
            m[QStringLiteral("entrypoints")] =
                QJsonObject{{QStringLiteral("mcp"), QStringLiteral("../../MacOS/evil.mjs")},
                            {QStringLiteral("jsonrpc"), QStringLiteral("src/cli.mjs")}};
        }));
        QVERIFY(rejects(
            [](QJsonObject& m) { m[QStringLiteral("skill")] = QStringLiteral("/etc/hosts"); }));
        QVERIFY(rejects([](QJsonObject& m) {
            m[QStringLiteral("entrypoints")] =
                QJsonObject{{QStringLiteral("mcp"), QStringLiteral("src/missing.mjs")},
                            {QStringLiteral("jsonrpc"), QStringLiteral("src/cli.mjs")}};
        }));

        {
            // A symlinked entrypoint that resolves outside the runtime root is rejected.
            FakeBundle bundle;
            bundle.create();
            const QString outside = bundle.root.filePath(QStringLiteral("outside.mjs"));
            writeFile(outside);
            const QString link = bundle.runtime() + QStringLiteral("/src/mcp.mjs");
            QVERIFY(QFile::remove(link));
            QVERIFY(QFile::link(outside, link));
            QVERIFY(QFileInfo(link).isFile());
            QVERIFY(!loadBundledBrowserRuntime(bundle.runtime()).has_value());
        }
        {
            // Manifests over 64 KiB are not parsed, even when otherwise valid.
            FakeBundle bundle;
            bundle.create();
            QJsonObject manifest = validManifest();
            manifest[QStringLiteral("padding")] = QString(70 * 1024, QLatin1Char('x'));
            bundle.writeManifest(manifest);
            QVERIFY(QFileInfo(bundle.runtime() + QStringLiteral("/runtime.json")).size() >
                    64 * 1024);
            QVERIFY(!loadBundledBrowserRuntime(bundle.runtime()).has_value());
            manifest.remove(QStringLiteral("padding"));
            bundle.writeManifest(manifest);
            QVERIFY(loadBundledBrowserRuntime(bundle.runtime()).has_value());
        }

        FakeBundle garbage;
        garbage.create();
        writeFile(garbage.runtime() + QStringLiteral("/runtime.json"), "{not json");
        QVERIFY(!loadBundledBrowserRuntime(garbage.runtime()).has_value());
    }

    void providerResolvesMcpLaunch() {
        FakeBundle bundle;
        bundle.create();
        const QString cacheDir =
            bundle.root.filePath(QStringLiteral("data/browser-runtime/chrome"));
        const BundledRuntimeProvider provider(bundle.macos(), cacheDir);
        QVERIFY(!provider.resolve(QStringLiteral("other.tool"), QLatin1String(mcpStdioProtocol)));
        QVERIFY(!provider.resolve(QLatin1String(browserToolId), QStringLiteral("jsonrpc")));

        const auto launch =
            provider.resolve(QLatin1String(browserToolId), QLatin1String(mcpStdioProtocol));
        QVERIFY(launch.has_value());
        const QString root = QFileInfo(bundle.runtime()).canonicalFilePath();
        QCOMPARE(launch->command, root + QStringLiteral("/bin/node"));
        QCOMPARE(launch->args, QStringList{root + QStringLiteral("/src/mcp.mjs")});
        QCOMPARE(launch->workingDirectory, root);
        QCOMPARE(launch->env.value(QStringLiteral("KETPLUS_BROWSER_CACHE")), cacheDir);
        QVERIFY(QFileInfo(cacheDir).isDir());
        QCOMPARE(QFile::permissions(cacheDir) &
                     (QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup |
                      QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther),
                 QFileDevice::Permissions{});
        // Inherited NODE_OPTIONS and Playwright download hosts are overridden with empty values.
        for (const auto* name :
             {"NODE_OPTIONS", "PLAYWRIGHT_DOWNLOAD_HOST", "PLAYWRIGHT_CHROMIUM_DOWNLOAD_HOST"}) {
            QVERIFY2(launch->env.contains(QLatin1String(name)), name);
            QCOMPARE(launch->env.value(QLatin1String(name)), QString());
        }
        QCOMPARE(launch->packageVersion, QStringLiteral("0.1.0"));
        QCOMPARE(launch->source, QStringLiteral("bundled"));

        const QStringList config = mcpServerConfigArguments(*launch);
        QVERIFY(config.join(QLatin1Char(' ')).contains(QStringLiteral("KETPLUS_BROWSER_CACHE")));
        QVERIFY(config.join(QLatin1Char(' ')).contains(QStringLiteral("NODE_OPTIONS=\"\"")));

        const BundledRuntimeProvider missing(bundle.root.path());
        QVERIFY(!missing.resolve(QLatin1String(browserToolId), QLatin1String(mcpStdioProtocol)));
    }

    void bundledSkillIsAvailableOutsideSourceCheckout() {
        FakeBundle bundle;
        bundle.create();
        QTemporaryDir workspace;
        Settings settings;
        const QString expected =
            QFileInfo(bundle.runtime() + QStringLiteral("/.claude/skills/ketplus-browser/SKILL.md"))
                .canonicalFilePath();
        QCOMPARE(browserSkillFile(settings, workspace.path(), bundle.macos()), expected);
        // Match runtime precedence even if a saved checkout path is stale.
        settings.browserAppPath = workspace.filePath(QStringLiteral("missing"));
        QCOMPARE(browserSkillFile(settings, workspace.path(), bundle.macos()), expected);
        settings.enabled = false;
        QVERIFY(browserSkillFile(settings, workspace.path(), bundle.macos()).isEmpty());
    }

    void defaultSkillsAreFoundFromDesktopAndDaemonDirectories() {
        using namespace ketplus::agent_skills;
        FakeBundle bundle;
        QDir().mkpath(bundle.macos());
        QDir().mkpath(bundle.helpers());
        QVERIFY(bundledSkills(bundle.macos()).isEmpty());
        QVERIFY(defaultSkillInstructions(bundledSkills(bundle.macos())).isEmpty());

        const QString skills = bundle.resources() + QStringLiteral("/agent-skills");
        writeFile(skills + QStringLiteral("/ket-design-system/SKILL.md"), "---\n");
        writeFile(skills + QStringLiteral("/ket-atlas/SKILL.md"), "---\n");
        // A stale directory from an older bundle must not restore the old invocation name.
        writeFile(skills + QStringLiteral("/ketatlas/SKILL.md"), "---\n");
        // Not shipped by KetPlus, and a link out of the bundle is not a shipped skill.
        writeFile(skills + QStringLiteral("/other/SKILL.md"), "---\n");
        QTemporaryDir outside;
        writeFile(outside.filePath(QStringLiteral("SKILL.md")), "---\n");
        QVERIFY(QDir().mkpath(skills + QStringLiteral("/enterprise-grade")));
        QVERIFY(QFile::link(outside.filePath(QStringLiteral("SKILL.md")),
                            skills + QStringLiteral("/enterprise-grade/SKILL.md")));

        for (const QString& appDir : {bundle.macos(), bundle.helpers()}) {
            const auto found = bundledSkills(appDir);
            QCOMPARE(found.size(), 2);
            QCOMPARE(found.at(0).name, QStringLiteral("ket-atlas"));
            QCOMPARE(found.at(0).file,
                     QFileInfo(skills + QStringLiteral("/ket-atlas/SKILL.md")).canonicalFilePath());
            QCOMPARE(found.at(1).name, QStringLiteral("ket-design-system"));
        }

        const QString text = defaultSkillInstructions(bundledSkills(bundle.macos()));
        QVERIFY(text.startsWith(QStringLiteral("[KetPlus default skills]\n")));
        QVERIFY(text.endsWith(QStringLiteral("\n[/KetPlus default skills]")));
        QVERIFY(text.contains(QStringLiteral("- ket-atlas (%1): when the user asks for a mock")
                                  .arg(bundledSkills(bundle.macos()).at(0).file)));
        QVERIFY(text.contains(QStringLiteral("has not named or supplied a design system")));
        QVERIFY(!text.contains(QStringLiteral("enterprise-grade")));
        QVERIFY(!text.contains(QStringLiteral("/other/")));
    }

    void defaultCacheDirIsUnderKetPlusData() {
        QVERIFY(defaultBrowserCacheDir().endsWith(
            QStringLiteral("/Ketsuite/KetPlus/browser-runtime/chrome")));
    }
};

QTEST_GUILESS_MAIN(BundledBrowserRuntimeTest)
#include "BundledBrowserRuntimeTest.moc"

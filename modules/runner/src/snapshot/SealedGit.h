#pragma once

#include "snapshot/SnapshotResult.h"

#include <QByteArray>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

// The one way the snapshot library runs git: a child process with an environment built from
// nothing, so no system, global or repository-supplied configuration, hook, filter driver,
// fsmonitor command or credential helper can run, and no caller variable such as GIT_DIR
// leaks in. Every call names its repository with GIT_DIR explicitly; the library never lets
// git discover one, and it never points GIT_DIR at a user's repository.
namespace ketplus::runner::snapshot {

// Which git binary to run and how long one command may take.
struct GitTool {
    QString program = QStringLiteral("git");
    int timeoutMs = 10 * 60 * 1000;
};

// GIT_CONFIG_GLOBAL and GIT_CONFIG_SYSTEM arrived in Git 2.32.
inline constexpr int minimumGitMajor = 2;
inline constexpr int minimumGitMinor = 32;
[[nodiscard]] QString minimumGitVersion();

// Parses `git version 2.50.1 (Apple Git-155)`; false when the text is not a version.
[[nodiscard]] bool parseGitVersion(const QString& text, int* major, int* minor);

struct GitRun {
    QString gitDir;   // GIT_DIR; required.
    QString workTree; // GIT_WORK_TREE; empty for none.
    QString indexFile;
    QStringList arguments;
    QByteArray input;             // written to stdin, then stdin closes.
    QString inputFile;            // or stdin read from this file.
    QString outputFile;           // stdout goes to this file instead of the result.
    QStringList extraEnvironment; // NAME=value pairs, e.g. a fixed author identity.
    QStringList extraOptions;     // further -c NAME=value pairs, after the sealed ones
};

struct GitOutput {
    QByteArray out;
    QByteArray err;
};

class SealedGit {
  public:
    explicit SealedGit(GitTool tool = {});

    // Runs `git --version` and checks it against the minimum.
    [[nodiscard]] Result<QString> checkVersion() const;
    // Exit 0 → output; anything else → GitFailed with stderr (or GitUnavailable).
    [[nodiscard]] Result<GitOutput> run(const GitRun& run) const;
    // `git init --bare` with the sealed environment.
    [[nodiscard]] Result<GitOutput> initBare(const QString& path,
                                             const QString& objectFormat = {}) const;

    // The one git command the library runs against a user's repository: a read-only
    // `git config --get-regexp` with the user's global configuration in view (`home`,
    // `xdgConfigHome`, `globalConfig` as the user's git would see them; empty means unset),
    // so git itself resolves include, includeIf, precedence and `~`. System configuration
    // stays excluded. It writes nothing and runs no hook.
    [[nodiscard]] Result<GitOutput> queryUserConfig(const QString& gitDir, const QString& workTree,
                                                    const QString& home,
                                                    const QString& xdgConfigHome,
                                                    const QString& globalConfig,
                                                    const QString& keyPattern) const;

    [[nodiscard]] QProcessEnvironment environment() const;
    [[nodiscard]] static QStringList sealedOptions();

  private:
    [[nodiscard]] Result<GitOutput> start(const QStringList& arguments,
                                          const QProcessEnvironment& environment,
                                          const GitRun& io) const;

    GitTool m_tool;
};

// A strict hex object id of 40 (SHA-1) or 64 (SHA-256) characters. Rejecting anything else
// keeps caller input from reaching git as an option or a revision expression.
[[nodiscard]] bool isObjectId(const QString& id);

} // namespace ketplus::runner::snapshot

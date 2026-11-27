#pragma once

#include <QElapsedTimer>
#include <QFileInfo>
#include <QString>

namespace ketplus::daemon {

// The path of a provider CLI. An explicit path is kept as given. A discovered path is looked up
// again when there is none or the file has gone, so a CLI installed while the daemon runs, or one
// whose updater replaced its versioned binary, is used on the next request without a restart.
class ProviderExecutable final {
  public:
    using Finder = QString (*)();

    // Looking up again scans PATH and a few well-known locations; status polls may ask often.
    static constexpr qint64 rediscoveryIntervalMs = 2000;

    ProviderExecutable(const QString& explicitPath, Finder finder)
        : finder_(explicitPath.isEmpty() ? finder : nullptr),
          path_(explicitPath.isEmpty() ? finder() : explicitPath) {
        lastLookup_.start();
    }

    [[nodiscard]] const QString& path() const {
        if (finder_ != nullptr && (path_.isEmpty() || !QFileInfo::exists(path_)) &&
            lastLookup_.elapsed() >= rediscoveryIntervalMs) {
            path_ = finder_();
            lastLookup_.restart();
        }
        return path_;
    }

    // Tests and callers that just installed the CLI can skip the interval.
    void rediscoverNow() {
        if (finder_ != nullptr) {
            path_ = finder_();
            lastLookup_.restart();
        }
    }

  private:
    Finder finder_;
    mutable QString path_;
    mutable QElapsedTimer lastLookup_;
};

} // namespace ketplus::daemon

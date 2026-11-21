#pragma once

#include <QObject>
#include <QString>

class QFileSystemWatcher;

namespace ketplus {
struct ThemeSelection {
    bool followSystem{true};
    QString schemeId{QStringLiteral("builtin-light")};
    QString lightSchemeId{QStringLiteral("builtin-light")};
    QString darkSchemeId{QStringLiteral("builtin-dark")};
    bool operator==(const ThemeSelection&) const = default;
};

// Committed state only. Previews never reach this file or advance its revision.
class ThemeSelectionStore final : public QObject {
    Q_OBJECT
  public:
    explicit ThemeSelectionStore(QString directory = {},
                                 QString legacyTheme = QStringLiteral("system"),
                                 QObject* parent = nullptr);
    [[nodiscard]] const ThemeSelection& selection() const;
    [[nodiscard]] quint64 revision() const;
    [[nodiscard]] QString filePath() const;
    [[nodiscard]] QString lastError() const;
    [[nodiscard]] bool commit(const ThemeSelection& selection);
    // Call on application activation as a fallback for missed directory events.
    void refresh();

  signals:
    void changed();
    void failed(const QString& message);

  private:
    void fail(const QString& message);
    QString directory_;
    QFileSystemWatcher* watcher_;
    ThemeSelection selection_;
    quint64 revision_{0};
    QString lastError_;
};
} // namespace ketplus

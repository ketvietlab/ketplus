#pragma once

#include <QObject>
#include <QHash>
#include <QList>
#include <QSet>
#include <QString>

#include <optional>

class QProcess;

namespace ketplus {

class MermaidRenderer final : public QObject {
    Q_OBJECT

  public:
    enum class State {
        Unavailable,
        Pending,
        Ready,
        Failed,
    };

    struct Result final {
        State state{State::Unavailable};
        QString cacheKey;
        QString filePath;
        QString error;
    };

    explicit MermaidRenderer(QObject* parent = nullptr);
    ~MermaidRenderer() override;

    [[nodiscard]] bool isAvailable() const noexcept;
    [[nodiscard]] const QString& executablePath() const noexcept;
    [[nodiscard]] Result request(const QString& source, bool darkTheme, bool raster = false);
    // Rewrites mermaid-cli's multi-row SVG labels into one <text> per row with pixel offsets.
    // Qt SVG ignores em-based `y`/`dy` and nested <tspan>, so the rows mermaid emits with
    // htmlLabels off would otherwise sit at the top of their shapes or disappear.
    [[nodiscard]] static QByteArray flattenLabelRows(const QByteArray& svg);

  signals:
    void diagramReady(const QString& cacheKey);
    void diagramFailed(const QString& cacheKey, const QString& error);

  private:
    struct Job final {
        QString cacheKey;
        QString source;
        QString outputPath;
        bool darkTheme{false};
    };

    [[nodiscard]] static QString discoverExecutable();
    [[nodiscard]] static QString keyFor(const QString& source, bool darkTheme, bool raster);
    [[nodiscard]] QString outputPathFor(const QString& cacheKey, bool raster) const;
    [[nodiscard]] static QString partialPathFor(const QString& outputPath);
    void startNext();
    void finishCurrent(bool succeeded, const QString& error = {});

    QString executablePath_;
    QString cacheDirectory_;
    QProcess* process_{nullptr};
    QList<Job> queue_;
    QSet<QString> queuedKeys_;
    QHash<QString, QString> failures_;
    std::optional<Job> currentJob_;
};

} // namespace ketplus

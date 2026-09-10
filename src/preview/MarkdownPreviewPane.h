#pragma once

#include "ui/Theme.h"

#include <QHash>
#include <QImage>
#include <QUrl>
#include <QWidget>

class QLabel;
class QResizeEvent;
class QTimer;

namespace ketplus {

class MermaidRenderer;
class PreviewBrowser;

class MarkdownPreviewPane final : public QWidget {
    Q_OBJECT

  public:
    explicit MarkdownPreviewPane(QWidget* parent = nullptr);

    void setSource(const QString& markdown, const QString& filePath,
                   const ThemePalette& palette);
    void showEmpty(const ThemePalette& palette);

  signals:
    void closeRequested();
    void fileOpenRequested(const QString& filePath);
    void statusMessageRequested(const QString& message);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    struct PreparedDocument final {
        QString markdown;
        QHash<QUrl, QImage> images;
        QHash<QString, QString> diagramFiles;
        bool rendererUnavailable{false};
        QString rendererError;
    };

    void renderNow();
    [[nodiscard]] PreparedDocument prepareDocument();
    [[nodiscard]] QImage renderSvg(const QString& filePath) const;
    void updateDocumentMargin();
    void applyDocumentStyle();
    void applyBlockTypography();
    void linkMermaidImages();
    void setNotice(const QString& message, bool error = false);

    PreviewBrowser* browser_{nullptr};
    QLabel* notice_{nullptr};
    QTimer* renderTimer_{nullptr};
    MermaidRenderer* mermaidRenderer_{nullptr};
    QString markdown_;
    QString filePath_;
    QString lastStatusMessage_;
    ThemePalette palette_;
    QHash<QString, QString> diagramFiles_;
    bool hasSource_{false};
};

} // namespace ketplus

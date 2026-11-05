#pragma once
#include "atlas/AtlasProcess.h"
#include "atlas/AtlasThumbnailCache.h"
#include <QPointer>
#include <QTimer>
#include <QWidget>

class QListWidget;
class QLineEdit;
class QLabel;
class QPalette;
class QProcess;
class QPushButton;
class QStackedWidget;
class QWebView;

namespace ketplus {
class AtlasTab final : public QWidget {
    Q_OBJECT
  public:
    explicit AtlasTab(AtlasIndex* index, QWidget* parent = nullptr);
    ~AtlasTab() override;
    QString atlasPath() const { return atlas_.path; }
    QString atlasId() const { return atlas_.path.isEmpty() ? QString{} : atlas_.atlasId(); }
    QString preferredFlowId() const { return preferredFlowId_; }
    bool isDeferred() const { return deferred_; }
    void openAtlas(const AtlasDescriptor& atlas);
    void refreshPreview();
    void applyPalette(const QPalette& palette, int interfaceFontSizePixels = 14);
    void setActive(bool active);
    void defer();
    void setDeferInterval(int milliseconds) { idle_.setInterval(milliseconds); }
  signals:
    void atlasRequested(const AtlasDescriptor& atlas);
    void creationRequested();
    void titleChanged(const QString& title);
    void loaded();
    void flowContextChanged(const QString& atlasId, const QString& flowId,
                            const QString& flowTitle);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void populate();
    void launch();
    void installKetAtlas();
    void handleIndexChanged();
    void ensureVisibleThumbnails();
    void probeFlowContext();
    void returnToLibrary();
    void releaseBrowser();
    void showError(const QString& error);
    void showMissingCli();
    AtlasIndex* index_;
    AtlasDescriptor atlas_;
    AtlasProcess process_;
    QTimer idle_;
    QTimer restoreTimer_;
    QLineEdit* search_;
    QListWidget* grid_;
    QLabel* count_;
    QLabel* scope_;
    QLabel* empty_;
    QLabel* status_;
    QLabel* installCommand_;
    QLabel* lastValidNotice_;
    QPushButton* retry_;
    QPushButton* locate_;
    QPushButton* install_;
    QPushButton* chooseAnother_;
    QStackedWidget* pages_;
    QWidget* picker_;
    QWidget* messagePage_;
    QWidget* browserContainer_{nullptr};
    QWebView* browser_{nullptr};
    QProcess* installer_{nullptr};
    QByteArray installOutput_;
    QString savedQuery_;
    QVariantMap savedState_;
    AtlasThumbnailCache thumbnails_;
    QTimer flowProbe_;
    QTimer missingBundle_;
    QString preferredFlowId_;
    bool deferred_{false};
    bool active_{true};
    bool applyingStyle_{false};
};
} // namespace ketplus

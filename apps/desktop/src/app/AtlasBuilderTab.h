#pragma once

#include "workspace/AtlasIndex.h"

#include <QFileSystemWatcher>
#include <QSet>
#include <QTimer>
#include <QVariantMap>
#include <QWidget>

class QFrame;
class QLabel;
class QPalette;
class QAbstractButton;

namespace ketplus {

class AgentWorkspaceView;
class AtlasTab;

class AtlasBuilderTab final : public QWidget {
    Q_OBJECT

  public:
    explicit AtlasBuilderTab(AtlasIndex* index, const QString& workspaceRoot,
                             QWidget* parent = nullptr);

    AgentWorkspaceView* chatView() const { return chat_; }
    QString workspaceRoot() const { return workspaceRoot_; }
    QString conversationId() const { return conversationId_; }
    QString atlasPath() const;
    QVariantMap promptContext() const;
    bool hasBuiltAtlas() const { return built_; }

    void setConversationId(const QString& conversationId);
    void openExistingAtlas(const AtlasDescriptor& atlas);
    void applyPalette(const QPalette& palette, int interfaceFontSizePixels = 14);
    void setActive(bool active);

  signals:
    void titleChanged(const QString& title);
    void conversationRequested(const QString& workspaceRoot, const QString& conversationId);
    void atlasBuilt(const AtlasDescriptor& atlas);

  protected:
    void resizeEvent(QResizeEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void detectCreatedAtlas();
    void updateGeometry();
    void updatePlaceholderTextHeights();
    void setChatCollapsed(bool collapsed);
    void watchBundle();
    void refreshPreview();
    void updateAiButton();
    void observeMessages();

    AtlasIndex* index_{nullptr};
    AtlasTab* atlas_{nullptr};
    QWidget* placeholder_{nullptr};
    QWidget* placeholderContent_{nullptr};
    QLabel* placeholderIcon_{nullptr};
    QLabel* explanation_{nullptr};
    QLabel* guidance_{nullptr};
    QLabel* samplePrompt_{nullptr};
    QLabel* flowContext_{nullptr};
    QFrame* chatFrame_{nullptr};
    AgentWorkspaceView* chat_{nullptr};
    QAbstractButton* collapse_{nullptr};
    QWidget* toolbar_{nullptr};
    QAbstractButton* back_{nullptr};
    QAbstractButton* aiButton_{nullptr};
    QTimer activityPulse_;
    bool pulseBright_{true};
    int interfaceFontSizePixels_{14};
    QMetaObject::Connection messageChanged_;
    QMetaObject::Connection messageCountChanged_;
    QSet<QString> initialAtlasPaths_;
    QString workspaceRoot_;
    QString conversationId_;
    QString flowId_;
    QString flowTitle_;
    bool built_{false};
    bool active_{true};
    bool chatCollapsed_{false};
    bool applyingStyle_{false};
    QFileSystemWatcher bundleWatcher_;
    QTimer previewRefresh_;
    QTimer bundleVerification_;
    quint64 bundleFingerprint_{0};
};

} // namespace ketplus

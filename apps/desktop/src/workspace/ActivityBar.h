#pragma once

#include "workspace/SidebarRepositoryList.h"

#include <QByteArray>
#include <QStringList>
#include <QVariantMap>
#include <QVector>
#include <QWidget>

class QButtonGroup;
class QComboBox;
class QLabel;
class QMenu;
class QResizeEvent;
class QToolButton;

namespace ketplus {

struct ThemePalette;

class ActivityBar final : public QWidget {
    Q_OBJECT

  public:
    enum class Mode { Explorer, Search, SourceControl, AgentWorkspace, Atlas, Remote, Settings };
    Q_ENUM(Mode)

    static constexpr int kWidth = 214;
    static constexpr int kMinWidth = 180;
    static constexpr int kMaxWidth = 360;

    explicit ActivityBar(QWidget* parent = nullptr);

    void setMode(Mode mode);
    void setSidebarWidth(int width);
    [[nodiscard]] int sidebarWidth() const noexcept { return sidebarWidth_; }
    void setSourceControlAvailable(bool available);
    void setSourceControlCount(int count);
    void setRepositories(const QVector<SidebarRepository>& repositories);
    void setMachines(const QStringList& machineIds, const QStringList& names,
                     const QString& selectedId);
    void setRemoteMachineSelected(bool selected);
    // Shows the "Update ready" reminder above the identity button; empty hides it.
    void setPendingUpdate(const QString& version);
    void applyPalette(const ThemePalette& palette, int interfaceFontSizePixels = 14);
    [[nodiscard]] Mode mode() const noexcept;

  signals:
    void modeRequested(ketplus::ActivityBar::Mode mode);
    void quickSearchRequested();
    void repositoryRequested();
    void machineRequested(const QString& machineId);
    void repositoryActivationRequested(const QString& path);
    void newConversationRequested(const QString& repositoryPath);
    void conversationRenameRequested(const QString& worktreePath, const QString& conversationId);
    void conversationRequested(const QString& worktreePath, const QString& conversationId);
    void conversationContextMenuRequested(const QString& worktreePath,
                                          const QString& conversationId);
    void settingsRequested();
    void identityRequested();
    void updatesRequested();
    void sidebarWidthChanged(int width);

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    QToolButton* addModeButton(const QByteArray& svgBody, const QString& label, Mode mode);
    void refreshIcons();

    QButtonGroup* group_{nullptr};
    QToolButton* quickSearchButton_{nullptr};
    QLabel* brandWordmark_{nullptr};
    QLabel* quickSearchShortcut_{nullptr};
    QToolButton* explorerButton_{nullptr};
    QToolButton* sourceControlButton_{nullptr};
    QLabel* sourceControlCount_{nullptr};
    QToolButton* agentButton_{nullptr};
    QToolButton* remoteButton_{nullptr};
    SidebarRepositoryList* repositoryList_{nullptr};
    QComboBox* machineSelect_{nullptr};
    QToolButton* repositoryAction_{nullptr};
    QLabel* machineNotice_{nullptr};
    QToolButton* updateReadyButton_{nullptr};
    QToolButton* identityButton_{nullptr};
    QMenu* userMenu_{nullptr};
    QWidget* resizeHandle_{nullptr};
    int sidebarWidth_{kWidth};
    QString iconMainColor_;
    QString iconMutedColor_;
    QString iconDisabledColor_;
    QString iconAccentColor_;
    QString iconPositiveColor_;
    QString iconWarningColor_;
    QString iconDangerColor_;
    Mode mode_{Mode::Explorer};
};

} // namespace ketplus

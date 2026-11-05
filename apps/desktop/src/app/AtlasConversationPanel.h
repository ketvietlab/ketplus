#pragma once

#include "workspace/AtlasIndex.h"

#include <QWidget>

class QLineEdit;
class QLabel;
class QProcess;
class QVBoxLayout;

namespace ketplus {

class AtlasConversationPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit AtlasConversationPanel(AtlasIndex* index, QWidget* parent = nullptr);
    void applyPalette(const QPalette& palette);

  signals:
    void editRequested(const AtlasDescriptor& atlas);
    void newRequested();

  private:
    void render();
    void addAtlasRow(QVBoxLayout* layout, const AtlasDescriptor& atlas, bool elsewhere);
    void installCli(const AtlasDescriptor& atlas);

    AtlasIndex* index_;
    QLineEdit* search_;
    QLabel* count_;
    QWidget* rows_;
    QVBoxLayout* rowsLayout_;
    QProcess* installer_{nullptr};
};

} // namespace ketplus

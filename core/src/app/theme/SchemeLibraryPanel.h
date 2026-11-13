#pragma once
#include "app/theme/SchemeLibrary.h"
#include <QWidget>

class QVBoxLayout;
namespace ket {
class Banner;
class Button;
class TextField;
} // namespace ket
namespace ketplus {
class ThemeManager;
// Catalog management is independent of the host's committed/preview theme state.
class SchemeLibraryPanel final : public QWidget {
    Q_OBJECT
  public:
    explicit SchemeLibraryPanel(QString directory = {}, QWidget* parent = nullptr,
                                ThemeManager* theme = nullptr);
    [[nodiscard]] QString selectedId() const { return selectedId_; }
    void reload();
    // Parses and opens review; useful for drag/drop and the native file chooser alike.
    void inspectFile(const QString& path);
  signals:
    void schemeActivated(const QString& id);
    void previewRequested(const ketplus::ColorScheme& scheme);
    void previewFinished();
    void schemeRemoved(const QString& id);
    void catalogChanged();

  private:
    void rebuildRows();
    void syncSelection();
    void rebuildPreferences();
    bool selectScheme(const QString& id);
    bool prepareRemoval(const ColorScheme& scheme);
    void chooseImport();
    void reviewCandidates(const QList<SchemeImportCandidate>& candidates);
    void chooseVariants(const SchemeImportBatch& batch);
    void duplicateSelected();
    void exportSelected();
    void deleteSelected();
    void showError(const QString& message);
    SchemeLibrary library_;
    ThemeManager* theme_;
    QWidget* preferences_{nullptr};
    QList<ColorScheme> schemes_;
    QString selectedId_;
    ket::TextField* search_;
    QWidget* rows_;
    QVBoxLayout* rowLayout_;
    ket::Banner* notice_;
    ket::Button* export_;
    ket::Button* duplicate_;
    ket::Button* remove_;
    quint64 inspectionGeneration_{0};
};
} // namespace ketplus

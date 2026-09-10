#pragma once

#include "editor/EditorSettings.h"

#include <QDialog>

class QFontComboBox;
class QSpinBox;

namespace ketplus {

class TypographyPreview;

class SettingsDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit SettingsDialog(const EditorSettings& settings, QWidget* parent = nullptr);

    [[nodiscard]] EditorSettings settings() const;

  signals:
    void settingsSaved(const ketplus::EditorSettings& settings);

  private:
    void setSettings(const EditorSettings& settings);
    void updatePreview();

    QFontComboBox* fontBox_{nullptr};
    QSpinBox* fontSizeBox_{nullptr};
    QSpinBox* lineHeightBox_{nullptr};
    TypographyPreview* preview_{nullptr};
};

} // namespace ketplus

#pragma once

#include "ui/Theme.h"

#include <QDialog>

namespace ketplus {

class MermaidDiagramDialog final : public QDialog {
    Q_OBJECT

  public:
    explicit MermaidDiagramDialog(const QString& svgPath, const ThemePalette& palette,
                                  QWidget* parent = nullptr);
};

} // namespace ketplus

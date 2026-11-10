#pragma once

#include <QWidget>

namespace ket {
class Badge;
}

namespace ketplus {

// The Appearance page's sample of the workspace, as in the mock: an editor tab strip, a
// highlighted code block in the editor font and a finished agent message. Everything is drawn
// from the kit's tab, code block and syntax recipes for the current theme.
class WorkspacePreview final : public QWidget {
    Q_OBJECT

  public:
    explicit WorkspacePreview(QWidget* parent = nullptr);

    void setCodeFamily(const QString& family);
    [[nodiscard]] QString codeFamily() const { return codeFamily_; }
    // The editor family at the code block's size.
    [[nodiscard]] QFont codeFont() const;

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    [[nodiscard]] int codeLineHeight() const;
    [[nodiscard]] int codeBlockHeight() const;

    QString codeFamily_;
    ket::Badge* badge_{nullptr};
};

} // namespace ketplus

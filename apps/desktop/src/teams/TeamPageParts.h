#pragma once

#include "KetButton.h"

#include <QAbstractButton>

#include <QFrame>
#include <QLabel>
#include <QLayout>
#include <QList>
#include <QString>
#include <QStyledItemDelegate>

class QTableWidget;

namespace ketplus::team_page {

// Logical pixels at the kit's zoom.
[[nodiscard]] int px(qreal logical);

// Text of the team page. A QLabel wraps at words only, so one long path or sentence makes the
// whole page wider than its viewport. This one never asks for more width than it is given:
// it wraps inside a word when it has to, or shows one elided line with the full text as tooltip.
class Text final : public QLabel {
    Q_OBJECT

  public:
    enum class Fit { Wrap, Elide, ElideMiddle };

    Text(const QString& value, QWidget* parent, Fit fit = Fit::Wrap);

    // A colour token of the kit, read when painting so a theme change needs no rebuild.
    void setTone(const QString& token);
    void setType(qreal pixels, int weight = QFont::Normal, bool mono = false);
    void setLetterSpacing(qreal pixels);
    // Wrapped text shows at most this many lines, the last one elided; 0 shows all of it.
    void setMaximumLines(int lines);
    [[nodiscard]] bool truncated() const { return truncated_; }

    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;
    [[nodiscard]] bool hasHeightForWidth() const override { return fit_ == Fit::Wrap; }
    [[nodiscard]] int heightForWidth(int width) const override;

  signals:
    // Whether the lines allowed hold the whole text changed, with the width or the limit.
    void truncatedChanged(bool truncated);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

  private:
    [[nodiscard]] qreal lay(int width, QPainter* painter, bool* cut = nullptr) const;
    void measure();

    Fit fit_;
    int maximumLines_{0};
    bool truncated_{false};
    QString tone_{QStringLiteral("text-main")};
};

// A bordered surface: the decision card and the lists of tasks, members and updates.
class Panel final : public QFrame {
    Q_OBJECT

  public:
    explicit Panel(QWidget* parent, bool accent = false);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    bool accent_;
};

// A row of a panel, with a rule above every row but the first.
class Row final : public QWidget {
    Q_OBJECT

  public:
    Row(QWidget* parent, bool ruled);

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    bool ruled_;
};

// Lays its items in a line and continues on the next line when they do not fit. An item wider
// than the layout is given the layout's width.
class Flow final : public QLayout {
    Q_OBJECT

  public:
    Flow(QWidget* parent, int horizontalGap, int verticalGap);
    ~Flow() override;

    void addItem(QLayoutItem* item) override;
    [[nodiscard]] int count() const override { return static_cast<int>(items_.size()); }
    [[nodiscard]] QLayoutItem* itemAt(int index) const override { return items_.value(index); }
    QLayoutItem* takeAt(int index) override;
    [[nodiscard]] Qt::Orientations expandingDirections() const override { return {}; }
    [[nodiscard]] bool hasHeightForWidth() const override { return true; }
    [[nodiscard]] int heightForWidth(int width) const override;
    void setGeometry(const QRect& rect) override;
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSize() const override;

  private:
    int place(const QRect& rect, bool apply) const;

    QList<QLayoutItem*> items_;
    int horizontalGap_;
    int verticalGap_;
};

// A decision's choice. A team writes its choices as sentences, so the label elides to the
// width the card gives it and keeps the whole sentence as its tooltip.
class ChoiceButton final : public ket::Button {
    Q_OBJECT

  public:
    ChoiceButton(const QString& label, ket::ButtonVariant variant, QWidget* parent);

    [[nodiscard]] QString label() const { return label_; }
    [[nodiscard]] QSize sizeHint() const override { return full_; }
    [[nodiscard]] QSize minimumSizeHint() const override;

  protected:
    void resizeEvent(QResizeEvent* event) override;

  private:
    QString label_;
    QSize full_;
};

// A decision's choice written as a sentence: a row of its own, wrapped, the recommended one
// marked in words. Short choices stay buttons (ChoiceButton).
class ChoiceRow final : public QAbstractButton {
    Q_OBJECT

  public:
    ChoiceRow(const QString& label, bool recommended, QWidget* parent);

  protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

  private:
    bool recommended_;
};

// Paints the task rows: a rule above each, the hovered and the current row marked, text elided.
// The State column shows a badge placed over the cell, so its text is not painted. A click or
// Return on a row chooses it.
class TaskRows final : public QStyledItemDelegate {
    Q_OBJECT

  public:
    explicit TaskRows(QTableWidget* table);

    static constexpr int kStateColumn = 5;

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override;
    // Hides the columns that do not fit, the least needed first.
    void fit();

  signals:
    void chosen(int row);

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    QTableWidget* table_;
    int hovered_{-1};
};

} // namespace ketplus::team_page

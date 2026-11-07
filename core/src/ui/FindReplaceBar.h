#pragma once

#include <QWidget>

class QAbstractButton;
class QLabel;
class QLineEdit;
class QWidget;

namespace ketplus {

class FindReplaceBar final : public QWidget {
    Q_OBJECT

  public:
    explicit FindReplaceBar(QWidget* parent = nullptr);

    [[nodiscard]] QString query() const;
    [[nodiscard]] QString replacement() const;
    [[nodiscard]] bool matchCase() const;
    [[nodiscard]] bool wholeWord() const;
    [[nodiscard]] bool regex() const;
    [[nodiscard]] bool inSelection() const;

    void open(bool showReplace, const QString& selectedText = {});
    void showSearchResult(bool found, bool wrapped);
    void showReplacementCount(int count);
    void showMatchCount(int count, bool limitReached);

  signals:
    void findRequested(bool backwards);
    void replaceRequested();
    void replaceAllRequested();
    void closeRequested();
    void queryChanged();
    void searchOptionsChanged();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void dismiss();

    QLineEdit* findEdit_{nullptr};
    QLineEdit* replaceEdit_{nullptr};
    QAbstractButton* matchCaseCheck_{nullptr};
    QAbstractButton* wholeWordCheck_{nullptr};
    QAbstractButton* regexCheck_{nullptr};
    QAbstractButton* inSelectionCheck_{nullptr};
    QLabel* resultLabel_{nullptr};
    QWidget* replaceRow_{nullptr};
};

} // namespace ketplus

#pragma once

#include <QWidget>

class QCheckBox;
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

    void open(bool showReplace, const QString& selectedText = {});
    void showSearchResult(bool found, bool wrapped);
    void showReplacementCount(int count);

  signals:
    void findRequested(bool backwards);
    void replaceRequested();
    void replaceAllRequested();
    void closeRequested();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void dismiss();

    QLineEdit* findEdit_{nullptr};
    QLineEdit* replaceEdit_{nullptr};
    QCheckBox* matchCaseCheck_{nullptr};
    QCheckBox* wholeWordCheck_{nullptr};
    QLabel* resultLabel_{nullptr};
    QWidget* replaceRow_{nullptr};
};

} // namespace ketplus

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>
#include <QWidget>

namespace ket {
class TextField;
} // namespace ket
class QVBoxLayout;

namespace ketplus {

namespace team_developers {
// T-4: names for `count` more developers of `team`: "Developer N" for the next numbers that no
// member's name and no developer's lane uses, after the developers it has. `alsoTaken` names the
// rows already chosen.
[[nodiscard]] QStringList nextNames(const QJsonObject& team, int count,
                                    const QStringList& alsoTaken = {});
} // namespace team_developers

// T-4: the developers one Add adds, a row each with its name and its lane (T-10: a lane is only a
// name). Rows start with the next free names and can be edited before they are added.
class TeamDeveloperRows final : public QWidget {
    Q_OBJECT

  public:
    explicit TeamDeveloperRows(const QJsonObject& team, QWidget* parent = nullptr);

    [[nodiscard]] int count() const { return static_cast<int>(rows_.size()); }
    // Keeps the rows already there, edited or not, and names the ones added.
    void setCount(int count);
    [[nodiscard]] ket::TextField* nameField(int row) const { return rows_.at(row).name; }
    [[nodiscard]] ket::TextField* laneField(int row) const { return rows_.at(row).lane; }
    // [{name, lane}], a lane left empty taking the developer's name.
    [[nodiscard]] QJsonArray developers() const;
    // What keeps the rows from being added, the fields concerned marked; empty when nothing.
    QString validate();

  private:
    struct Row final {
        QWidget* widget;
        ket::TextField* name;
        ket::TextField* lane;
    };

    QJsonObject team_;
    QVBoxLayout* layout_;
    QList<Row> rows_;
};

} // namespace ketplus

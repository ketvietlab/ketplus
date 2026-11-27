#include "teams/TeamDeveloperRows.h"

#include "KetTextField.h"

#include <QCoreApplication>
#include <QHBoxLayout>
#include <QSet>
#include <QVBoxLayout>

namespace ketplus {
namespace {

QString folded(const QString& text) { return text.trimmed().toCaseFolded(); }

// The team's member names and its developers' lanes, folded.
void taken(const QJsonObject& team, QSet<QString>* names, QSet<QString>* lanes) {
    for (const auto& value : team.value(QStringLiteral("members")).toArray()) {
        const QJsonObject member = value.toObject();
        names->insert(folded(member.value(QStringLiteral("name")).toString()));
        const QString lane = member.value(QStringLiteral("lane")).toString();
        if (member.value(QStringLiteral("role")).toString() == QStringLiteral("developer") &&
            !lane.trimmed().isEmpty())
            lanes->insert(folded(lane));
    }
}

} // namespace

QStringList team_developers::nextNames(const QJsonObject& team, const int count,
                                       const QStringList& alsoTaken) {
    QSet<QString> names;
    QSet<QString> lanes;
    taken(team, &names, &lanes);
    for (const QString& text : alsoTaken)
        names.insert(folded(text));
    int developers = 0;
    for (const auto& value : team.value(QStringLiteral("members")).toArray())
        if (value.toObject().value(QStringLiteral("role")).toString() ==
            QStringLiteral("developer"))
            ++developers;
    QStringList result;
    for (int number = developers + 1; result.size() < count; ++number) {
        const QString name =
            QCoreApplication::translate("TeamDeveloperRows", "Developer %1").arg(number);
        if (!names.contains(folded(name)) && !lanes.contains(folded(name)))
            result.append(name);
    }
    return result;
}

TeamDeveloperRows::TeamDeveloperRows(const QJsonObject& team, QWidget* parent)
    : QWidget(parent), team_(team), layout_(new QVBoxLayout(this)) {
    setObjectName(QStringLiteral("teamDeveloperRows"));
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(6);
}

void TeamDeveloperRows::setCount(const int count) {
    while (rows_.size() > qMax(0, count))
        delete rows_.takeLast().widget;
    if (rows_.size() >= count)
        return;
    // The names and lanes of the rows kept are taken too, so a new row never repeats one.
    QStringList kept;
    for (const Row& row : std::as_const(rows_))
        kept << row.name->text() << row.lane->text();
    const QStringList names =
        team_developers::nextNames(team_, count - static_cast<int>(rows_.size()), kept);
    for (const QString& name : names) {
        const int index = static_cast<int>(rows_.size()) + 1;
        auto* widget = new QWidget(this);
        auto* row = new QHBoxLayout(widget);
        row->setContentsMargins(0, 0, 0, 0);
        row->setSpacing(6);
        auto* nameField = new ket::TextField(widget);
        nameField->setObjectName(QStringLiteral("developerRowName%1").arg(index));
        nameField->setPlaceholderText(tr("Name"));
        nameField->setAccessibleName(tr("Name of developer %1").arg(index));
        nameField->setText(name);
        auto* laneField = new ket::TextField(widget);
        laneField->setObjectName(QStringLiteral("developerRowLane%1").arg(index));
        laneField->setPlaceholderText(tr("Lane"));
        laneField->setAccessibleName(tr("Lane of developer %1").arg(index));
        laneField->setText(name);
        for (ket::TextField* field : {nameField, laneField})
            connect(field, &QLineEdit::textEdited, field, [field] { field->setInvalid(false); });
        row->addWidget(nameField, 1);
        row->addWidget(laneField, 1);
        layout_->addWidget(widget);
        rows_.append({widget, nameField, laneField});
    }
}

QJsonArray TeamDeveloperRows::developers() const {
    QJsonArray result;
    for (const Row& row : rows_) {
        const QString name = row.name->text().trimmed();
        const QString lane = row.lane->text().trimmed();
        result.append(QJsonObject{{QStringLiteral("name"), name},
                                  {QStringLiteral("lane"), lane.isEmpty() ? name : lane}});
    }
    return result;
}

QString TeamDeveloperRows::validate() {
    QSet<QString> names;
    QSet<QString> lanes;
    taken(team_, &names, &lanes);
    QString problem;
    const auto refuse = [&problem](ket::TextField* field, const QString& message) {
        field->setInvalid(true);
        if (problem.isEmpty()) {
            problem = message;
            field->setFocus();
        }
    };
    for (const Row& row : std::as_const(rows_)) {
        row.name->setInvalid(false);
        row.lane->setInvalid(false);
        const QString name = row.name->text().trimmed();
        const QString lane =
            row.lane->text().trimmed().isEmpty() ? name : row.lane->text().trimmed();
        if (name.isEmpty())
            refuse(row.name, tr("Name each developer."));
        else if (names.contains(folded(name)))
            refuse(row.name, tr("%1 is already a name on the team.").arg(name));
        else if (!lane.isEmpty() && lanes.contains(folded(lane)))
            refuse(row.lane, tr("%1 is already a lane on the team.").arg(lane));
        names.insert(folded(name));
        lanes.insert(folded(lane));
    }
    return problem;
}

} // namespace ketplus

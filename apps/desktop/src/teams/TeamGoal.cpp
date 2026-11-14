#include "teams/TeamGoal.h"
#include "agent/ProviderDisplay.h"

#include "KetExpandableText.h"
#include "KetMarkdown.h"
#include "KetMentionTokens.h"
#include "KetTextArea.h"

#include <QCoreApplication>
#include <QSet>
#include <QTextDocument>
#include <QTextEdit>
#include <QVariantMap>

#include <utility>

namespace ketplus {
namespace team_goal {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("ketplus::TeamGoal", text); }

QString detail(const QVariantMap& conversation) {
    const QString provider = conversation.value(QStringLiteral("provider")).toString();
    QStringList parts{provider_display::name(provider)};
    if (conversation.value(QStringLiteral("running")).toBool())
        parts.append(tr("working"));
    if (const QString path = conversation.value(QStringLiteral("path")).toString(); !path.isEmpty())
        parts.append(path.section(QLatin1Char('/'), -1, -1, QString::SectionSkipEmpty));
    return parts.join(QStringLiteral(" · "));
}

// A plain `@` starts a mention where the kit's editor would draw one: at the start, after a
// space or after an opening bracket or quote.
bool startsMention(const QString& text, const int at) {
    if (at < 0 || at >= text.size() || text.at(at) != QLatin1Char('@'))
        return false;
    if (at > 0) {
        const QChar before = text.at(at - 1);
        if (!before.isSpace() && !QStringLiteral("([{'\"").contains(before))
            return false;
    }
    return !text.mid(at + 1).startsWith(QStringLiteral("agent/"));
}

} // namespace

QString placeholder() {
    return tr("What should the team reach? Add the context, links and issues the leader needs, "
              "and type @ to mention a conversation the leader can ask.");
}

ket::ExpandableText* foldedGoal(const QString& goal, const QVariantList& directory,
                                QWidget* parent) {
    auto* text = new ket::ExpandableText(parent);
    text->setObjectName(QStringLiteral("teamPageGoal"));
    text->setFoldedLines(foldedLines);
    for (const QVariant& value : directory) {
        const QVariantMap conversation = value.toMap();
        text->setMentionTitle(conversation.value(QStringLiteral("number")).toString(),
                              conversation.value(QStringLiteral("title")).toString());
    }
    text->setUnknownMentions(unknownMentions(goal, directory));
    text->setText(goal);
    return text;
}

QStringList unknownMentions(const QString& text, const QVariantList& directory) {
    QSet<QString> known;
    for (const QVariant& value : directory)
        known.insert(value.toMap().value(QStringLiteral("number")).toString());
    QStringList unknown;
    for (const ket::MentionSpan& span : ket::findAgentMentions(text))
        if (!known.contains(span.number) && !unknown.contains(span.number))
            unknown.append(span.number);
    return unknown;
}

QString problem(const QString& text, const QStringList& unknown) {
    if (text.size() > maximumCharacters)
        return tr("The goal has %L1 characters; a team keeps at most %L2. Shorten it, or link "
                  "to the details instead.")
            .arg(text.size())
            .arg(maximumCharacters);
    if (const qsizetype bytes = text.toUtf8().size(); bytes > maximumBytes)
        return tr("The goal is too long to reach the leader in one message: it takes %L1 of "
                  "%L2 bytes, as accented letters, other scripts and emoji take more than one "
                  "each. Shorten it, or link to the details instead.")
            .arg(bytes)
            .arg(maximumBytes);
    if (unknown.isEmpty())
        return {};
    QStringList names;
    for (const QString& number : unknown)
        names.append(QStringLiteral("@agent/") + number);
    return unknown.size() == 1
               ? tr("%1 is not a conversation on this computer. Correct the number or remove "
                    "the mention.")
                     .arg(names.first())
               : tr("%1 are not conversations on this computer. Correct the numbers or remove "
                    "the mentions.")
                     .arg(names.join(QStringLiteral(", ")));
}

} // namespace team_goal

TeamGoalEditor::TeamGoalEditor(ket::TextArea* field, team_goal::ConversationDirectory directory)
    : QObject(field), field_(field), directory_(std::move(directory)) {
    field->setObjectName(QStringLiteral("teamGoalField"));
    field->setMinimumLines(team_goal::visibleLines);
    field->setMaximumLines(team_goal::growLines);
    field->setMaxLength(team_goal::maximumCharacters);
    field->setPlaceholderText(team_goal::placeholder());
    field->setAccessibleName(tr("Goal"));
    connect(field, &ket::TextArea::mentionQueryChanged, this, &TeamGoalEditor::suggest);
    connect(field, &ket::TextArea::textChanged, this, &TeamGoalEditor::refresh);
    // One typed character: a plain `@` becomes `@agent/` once the edit is done.
    connect(field->editor()->document(), &QTextDocument::contentsChange, this,
            [this](const int position, const int removed, const int added) {
                if (removed == 0 && added == 1)
                    pendingAt_ = position;
            });
    refresh();
}

QString TeamGoalEditor::problem() const {
    return field_ ? team_goal::problem(field_->text(), unknown_) : QString{};
}

void TeamGoalEditor::suggest(const QString& query) {
    if (!field_)
        return;
    const QString needle = query.trimmed();
    QList<ket::MentionItem> items;
    for (const QVariant& value : directory_ ? directory_() : QVariantList{}) {
        const QVariantMap conversation = value.toMap();
        const QString number = conversation.value(QStringLiteral("number")).toString();
        const QString title = conversation.value(QStringLiteral("title")).toString();
        if (number.isEmpty() || (!needle.isEmpty() && !number.startsWith(needle) &&
                                 !title.contains(needle, Qt::CaseInsensitive)))
            continue;
        items.append({.number = number, .title = title, .place = team_goal::detail(conversation)});
        if (items.size() >= team_goal::maximumSuggestions)
            break;
    }
    field_->setMentionItems(items);
}

void TeamGoalEditor::completeMention(const int position) {
    QTextCursor cursor(field_->editor()->document());
    cursor.setPosition(position + 1);
    cursor.insertText(QStringLiteral("agent/"));
    field_->editor()->setTextCursor(cursor);
}

void TeamGoalEditor::refresh() {
    if (!field_)
        return;
    const QString text = field_->text();
    if (const int at = std::exchange(pendingAt_, -1); team_goal::startsMention(text, at)) {
        completeMention(at); // changes the text again, which refreshes
        return;
    }
    const QVariantList directory = directory_ ? directory_() : QVariantList{};
    for (const QVariant& value : directory) {
        const QVariantMap conversation = value.toMap();
        field_->mentions()->setTitle(conversation.value(QStringLiteral("number")).toString(),
                                     conversation.value(QStringLiteral("title")).toString());
    }
    const QStringList unknown = team_goal::unknownMentions(text, directory);
    if (unknown != unknown_) {
        unknown_ = unknown;
        field_->setUnknownMentions(unknown_);
    }
    const QString now = problem();
    field_->setInvalid(!unknown_.isEmpty() || text.toUtf8().size() > team_goal::maximumBytes);
    if (now != problem_) {
        problem_ = now;
        emit problemChanged(problem_);
    }
}

} // namespace ketplus

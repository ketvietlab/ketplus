#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QVariantList>
#include <QVariantMap>

namespace ketplus::agent_message {

// The box shown while a Mermaid diagram the reader asked for is being drawn, or
// cannot be (Mermaid missing). The finished diagram does not keep it: it takes
// its own height.
[[nodiscard]] qreal mermaidFrameHeight(qreal availableWidth);

QString persistedKey(const QString& path);
QVariantMap message(QString kind, QString body, QString meta = {}, QString filePath = {},
                    QString itemId = {});
QString firstLine(const QString& text, qsizetype maximum = 56);
QVariantMap fileChangeMessage(const QJsonObject& item, bool completed);
QString selectedSkillInstructions(const QVariantList& skills);
QString errorMessage(const QJsonObject& error);
QString itemSummary(const QJsonObject& item);
QVariantMap webSearchMessage(const QJsonObject& item, bool completed);
QVariantMap commandMessage(const QString& command, const QString& state, const QString& itemId);
QVariantMap commandMessage(const QJsonObject& item, bool completed);
QVariantMap assistantFinalMessage(const QJsonObject& item, bool completed);
QVariantMap activityMessage(const QJsonObject& item, bool completed);
bool mergeImageActivity(QVariantMap& existing, const QVariantMap& incoming, bool completed);
QVariantList historyMessages(const QJsonArray& turns, QSet<QString>& seenTurns,
                             const QString& sessionId = {}, quint64 generation = 1);
QString transcriptText(const QVariantList& entries);
// Source text copied for one transcript entry: the raw Markdown/plain body of user and assistant
// messages, and the command/target plus an outcome line for activity rows.
QString entryCopyText(const QVariantMap& entry);
// Contents of the fenced code blocks (``` or ~~~) of a Markdown source, in document order.
QStringList fencedCodeBlocks(const QString& markdown);
// The Markdown handed to QTextDocument::setMarkdown. Qt's importer folds a fenced code block
// inside a tight list item into the list: each code line became a numbered item and the code
// format ran on through every item after it. A blank line around such a fence makes the list
// loose, which Qt imports correctly. Everything else is returned unchanged.
QString qtMarkdownSource(const QString& markdown);

} // namespace ketplus::agent_message

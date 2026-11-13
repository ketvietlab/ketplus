#include "teams/TeamBranchNames.h"

#include <QRegularExpression>
#include <QSet>

namespace ketplus::daemon::teams::branch_names {

namespace {

// Words that carry no meaning in a branch name, in English.
const QSet<QString>& stopWords() {
    static const QSet<QString> words{
        // English
        QStringLiteral("a"),      QStringLiteral("an"),   QStringLiteral("the"),
        QStringLiteral("and"),    QStringLiteral("or"),   QStringLiteral("of"),
        QStringLiteral("to"),     QStringLiteral("for"),  QStringLiteral("in"),
        QStringLiteral("on"),     QStringLiteral("at"),   QStringLiteral("by"),
        QStringLiteral("with"),   QStringLiteral("from"), QStringLiteral("as"),
        QStringLiteral("is"),     QStringLiteral("are"),  QStringLiteral("be"),
        QStringLiteral("into"),   QStringLiteral("via"),  QStringLiteral("its"),
        QStringLiteral("it"),     QStringLiteral("this"), QStringLiteral("that"),
        QStringLiteral("when"),   QStringLiteral("then"), QStringLiteral("so"),
        QStringLiteral("all"),    QStringLiteral("each"), QStringLiteral("every"),
        QStringLiteral("should"), QStringLiteral("must"), QStringLiteral("can")};
    return words;
}

// Vietnamese ones with their diacritics: without them "đang" (is doing) would also drop
// "đăng" (post) and "để" (so that) "đề" (topic).
const QSet<QString>& vietnameseStopWords() {
    static const QSet<QString> words{
        QStringLiteral("và"),    QStringLiteral("của"),  QStringLiteral("cho"),
        QStringLiteral("với"),   QStringLiteral("là"),   QStringLiteral("các"),
        QStringLiteral("những"), QStringLiteral("một"),  QStringLiteral("để"),
        QStringLiteral("khi"),   QStringLiteral("thì"),  QStringLiteral("trong"),
        QStringLiteral("trên"),  QStringLiteral("theo"), QStringLiteral("từ"),
        QStringLiteral("ở"),     QStringLiteral("về"),   QStringLiteral("này"),
        QStringLiteral("đã"),    QStringLiteral("được"), QStringLiteral("bị"),
        QStringLiteral("cũng"),  QStringLiteral("sẽ"),   QStringLiteral("đang"),
        QStringLiteral("phải"),  QStringLiteral("hoặc"), QStringLiteral("vào")};
    return words;
}

// The words, lower case with their diacritics, that name a type when a title starts with one.
// Vietnamese words are matched with their diacritics: "lỗi" is a defect, "lời" is not.
QString typeOfWord(const QString& word) {
    static const QHash<QString, QString> words{
        {QStringLiteral("feat"), QStringLiteral("feat")},
        {QStringLiteral("feature"), QStringLiteral("feat")},
        {QStringLiteral("fix"), QStringLiteral("fix")},
        {QStringLiteral("bugfix"), QStringLiteral("fix")},
        {QStringLiteral("hotfix"), QStringLiteral("fix")},
        {QStringLiteral("sửa"), QStringLiteral("fix")},
        {QStringLiteral("lỗi"), QStringLiteral("fix")},
        {QStringLiteral("chore"), QStringLiteral("chore")},
        {QStringLiteral("ci"), QStringLiteral("chore")},
        {QStringLiteral("deps"), QStringLiteral("chore")},
        {QStringLiteral("bump"), QStringLiteral("chore")},
        {QStringLiteral("cleanup"), QStringLiteral("chore")},
        {QStringLiteral("dọn"), QStringLiteral("chore")},
        {QStringLiteral("docs"), QStringLiteral("docs")},
        {QStringLiteral("doc"), QStringLiteral("docs")},
        {QStringLiteral("document"), QStringLiteral("docs")},
        {QStringLiteral("adr"), QStringLiteral("docs")},
        {QStringLiteral("readme"), QStringLiteral("docs")},
        {QStringLiteral("refactor"), QStringLiteral("refactor")},
        {QStringLiteral("tách"), QStringLiteral("refactor")},
        {QStringLiteral("test"), QStringLiteral("test")},
        {QStringLiteral("tests"), QStringLiteral("test")},
        {QStringLiteral("kiểm"), QStringLiteral("test")},
        {QStringLiteral("thử"), QStringLiteral("test")}};
    return words.value(word);
}

bool namesDefect(const QString& word) {
    static const QSet<QString> words{QStringLiteral("fix"),        QStringLiteral("bug"),
                                     QStringLiteral("bugs"),       QStringLiteral("crash"),
                                     QStringLiteral("crashes"),    QStringLiteral("broken"),
                                     QStringLiteral("regression"), QStringLiteral("lỗi"),
                                     QStringLiteral("hỏng"),       QStringLiteral("sửa")};
    return words.contains(word);
}

QString withoutDiacritics(const QString& text) {
    QString lowered = text.toLower();
    lowered.replace(QChar(0x0111), QLatin1Char('d')); // đ
    const QString decomposed = lowered.normalized(QString::NormalizationForm_D);
    QString plain;
    plain.reserve(decomposed.size());
    for (const QChar character : decomposed)
        if (character.category() != QChar::Mark_NonSpacing)
            plain.append(character);
    return plain;
}

// Bracketed asides, such as "(ketplus-app #114)", say where, not what.
QString withoutAsides(const QString& title) {
    static const QRegularExpression asides(QStringLiteral("\\([^)]*\\)|\\[[^\\]]*\\]"));
    QString text = title;
    return text.replace(asides, QStringLiteral(" "));
}

// The words of a title, lower case with their diacritics: letters and digits only.
QStringList wordsOf(const QString& title) {
    static const QRegularExpression separators(QStringLiteral("[^\\p{L}\\p{N}]+"));
    return withoutAsides(title.normalized(QString::NormalizationForm_C))
        .toLower()
        .split(separators, Qt::SkipEmptyParts);
}

// The words that say what a task is: without task ids ("T-47", "t47") and bare numbers.
QStringList meaningfulWords(const QString& title) {
    static const QRegularExpression joined(QStringLiteral("^t\\d+$"));
    static const QRegularExpression number(QStringLiteral("^\\d+$"));
    const QStringList words = wordsOf(title);
    QStringList kept;
    for (qsizetype index = 0; index < words.size(); ++index) {
        const QString& word = words.at(index);
        if (number.match(word).hasMatch() || joined.match(word).hasMatch())
            continue;
        if (word == QStringLiteral("t") && index + 1 < words.size() &&
            number.match(words.at(index + 1)).hasMatch()) {
            ++index;
            continue;
        }
        kept.append(word);
    }
    return kept;
}

} // namespace

const QStringList& types() {
    static const QStringList all{QStringLiteral("feat"),     QStringLiteral("fix"),
                                 QStringLiteral("chore"),    QStringLiteral("docs"),
                                 QStringLiteral("refactor"), QStringLiteral("test")};
    return all;
}

QString typeOf(const QString& title) {
    const QStringList words = meaningfulWords(title);
    if (words.isEmpty())
        return QStringLiteral("feat");
    if (const QString first = typeOfWord(words.first()); !first.isEmpty())
        return first;
    for (const QString& word : words)
        if (namesDefect(word))
            return QStringLiteral("fix");
    return QStringLiteral("feat");
}

QString slugOf(const QString& title) {
    QStringList kept;
    bool leading = true;
    for (const QString& word : meaningfulWords(title)) {
        // "fix: …", "Sửa lỗi …": the type says it already.
        if (leading && !typeOfWord(word).isEmpty())
            continue;
        leading = false;
        const QString plain = withoutDiacritics(word);
        if (plain.size() < 2 || stopWords().contains(plain) || vietnameseStopWords().contains(word))
            continue;
        // Letters a branch cannot carry, such as ß or a CJK title, are left out.
        QString ascii;
        for (const QChar character : plain)
            if ((character >= QLatin1Char('a') && character <= QLatin1Char('z')) ||
                (character >= QLatin1Char('0') && character <= QLatin1Char('9')))
                ascii.append(character);
        if (ascii.size() >= 2)
            kept.append(ascii);
    }
    QString slug;
    for (const QString& word : kept) {
        const qsizetype length = slug.isEmpty() ? word.size() : slug.size() + 1 + word.size();
        if (length > maximumSlug) {
            if (slug.isEmpty())
                slug = word.left(maximumSlug);
            break;
        }
        slug = slug.isEmpty() ? word : slug + QLatin1Char('-') + word;
    }
    return slug.isEmpty() ? QStringLiteral("task") : slug;
}

std::optional<TeamError> validType(const QString& type) {
    if (types().contains(type))
        return std::nullopt;
    return TeamError{QStringLiteral("invalid_payload"),
                     QStringLiteral("\"%1\" is not a branch type: use %2.")
                         .arg(type, types().join(QStringLiteral(", ")))};
}

std::optional<TeamError> validSlug(const QString& slug) {
    static const QRegularExpression shape(QStringLiteral("^[a-z0-9]+(-[a-z0-9]+)*$"));
    if (slug.size() <= maximumSlug && shape.match(slug).hasMatch())
        return std::nullopt;
    return TeamError{
        QStringLiteral("invalid_payload"),
        QStringLiteral("\"%1\" is not a branch slug: lower-case letters and digits in words "
                       "joined by single hyphens, at most %2 characters.")
            .arg(slug)
            .arg(maximumSlug)};
}

int taskNumber(const QString& taskId) {
    static const QRegularExpression id(QStringLiteral("^T-(\\d+)$"));
    const auto match = id.match(taskId);
    return match.hasMatch() ? match.captured(1).toInt() : 0;
}

QString plannedBranch(const qint64 teamId, const Task& task) {
    if (task.branchSlug.isEmpty())
        return QStringLiteral("team/%1/%2").arg(teamId).arg(task.id.toLower());
    return QStringLiteral("%1/%2-t%3")
        .arg(task.branchType, task.branchSlug)
        .arg(taskNumber(task.id));
}

QString plannedWorktreeName(const QString& repositoryName, const qint64 teamId, const Task& task) {
    if (task.branchSlug.isEmpty())
        return QStringLiteral("%1-team-%2-%3")
            .arg(repositoryName)
            .arg(teamId)
            .arg(task.id.toLower());
    return QStringLiteral("%1-%2-%3-t%4")
        .arg(repositoryName, task.branchType, task.branchSlug)
        .arg(taskNumber(task.id));
}

} // namespace ketplus::daemon::teams::branch_names

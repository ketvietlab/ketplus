#include "TeamLanguage.h"

#include <QCoreApplication>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QThread>
#include <QTranslator>

namespace ketplus::remote::team_language {
namespace {

thread_local QString scoped;
// Read and written on the GUI thread only.
QString ambient;

QString active() {
    if (!scoped.isEmpty())
        return scoped;
    const auto* application = QCoreApplication::instance();
    return application && QThread::currentThread() == application->thread() ? ambient : QString{};
}

// The compiled translations, one per translated language, loaded on first use.
const QTranslator* catalog(const QString& language) {
    static QMutex mutex;
    static QHash<QString, QTranslator*> loaded;
    QMutexLocker lock(&mutex);
    if (const auto found = loaded.constFind(language); found != loaded.constEnd())
        return *found;
    auto* translator = new QTranslator;
    if (!translator->load(QStringLiteral(":/ketplus/i18n/ketplus_teams_%1.qm").arg(language))) {
        delete translator;
        translator = nullptr;
    }
    loaded.insert(language, translator);
    return translator;
}

// The team's texts share one context in the catalog (update_team_translations.py). The
// desktop asks under its class names, "ketplus::TeamPage", "TeamMenus", "ketplus::team_page::
// ChoiceRow", and only those are the team's; the rest of the application keeps its texts.
bool teamContext(const char* context) {
    QLatin1StringView name(context ? context : "");
    if (name.startsWith(QLatin1StringView("ketplus::")))
        name = name.sliced(9);
    return name.startsWith(QLatin1StringView("Team")) ||
           name.startsWith(QLatin1StringView("team_page::")) ||
           name == QLatin1StringView("NewTeamForm") || name == QLatin1StringView("AddMemberForm");
}

constexpr const char* catalogContext = "Team";

// Answers a team's tr() inside a Scope, or on the GUI thread while a team page is shown.
class ScopedTranslator final : public QTranslator {
  public:
    using QTranslator::QTranslator;
    [[nodiscard]] bool isEmpty() const override { return false; }
    [[nodiscard]] QString translate(const char* context, const char* source,
                                    const char* disambiguation, int n) const override {
        const QString language = active();
        if (language.isEmpty() || language == english || !teamContext(context))
            return {};
        const QTranslator* translations = catalog(language);
        return translations ? translations->translate(catalogContext, source, disambiguation, n)
                            : QString{};
    }
};

} // namespace

QStringList supported() { return {english, vietnamese}; }

bool isSupported(const QString& language) { return supported().contains(language); }

QString orEnglish(const QString& language) { return isSupported(language) ? language : english; }

QString fromLocale(const QString& tag) {
    const QString primary =
        tag.trimmed().section(QLatin1Char('-'), 0, 0).section(QLatin1Char('_'), 0, 0).toLower();
    return isSupported(primary) ? primary : english;
}

QString detect(const QString& text) {
    static const QRegularExpression vietnamese(
        QStringLiteral("[ăâđêôơưạảấầẩẫậắằẳẵặẹẻẽếềểễệỉịĩọỏốồổỗộớờởỡợụủũứừửữựỳỵỷỹ]"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::UseUnicodePropertiesOption);
    return vietnamese.match(text).hasMatch() ? team_language::vietnamese : QString{};
}

QString nativeName(const QString& language) {
    return language == vietnamese ? QStringLiteral("Tiếng Việt") : QStringLiteral("English");
}

QString text(const QString& language, const char* context, const char* source, const int n) {
    const QString plain =
        n < 0 ? QString::fromUtf8(source)
              : QString::fromUtf8(source).replace(QStringLiteral("%n"), QString::number(n));
    if (language.isEmpty() || language == english || !teamContext(context))
        return plain;
    const QTranslator* translations = catalog(language);
    QString translated =
        translations ? translations->translate(catalogContext, source, nullptr, n) : QString{};
    if (translated.isEmpty())
        return plain;
    // QCoreApplication::translate puts the count in; a translator alone leaves %n.
    return n < 0 ? translated : translated.replace(QStringLiteral("%n"), QString::number(n));
}

void install() {
    static ScopedTranslator* installed = [] {
        auto* translator = new ScopedTranslator;
        QCoreApplication::installTranslator(translator);
        return translator;
    }();
    (void)installed;
}

Scope::Scope(const QString& language) : previous_(scoped) {
    scoped = orEnglish(language);
    // The first translated Scope adds the translator, so no start-up step is needed.
    if (scoped != english && QCoreApplication::instance())
        install();
}

Scope::~Scope() { scoped = previous_; }

void setAmbient(const QString& language) {
    ambient = language.isEmpty() ? QString{} : orEnglish(language);
    if (ambient != english && !ambient.isEmpty() && QCoreApplication::instance())
        install();
}

QString current() {
    const QString language = active();
    return language.isEmpty() ? english : language;
}

} // namespace ketplus::remote::team_language

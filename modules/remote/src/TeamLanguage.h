#pragma once

// T-56: a team's language. A team stores the language its leader works in ("en" or "vi"),
// and every fixed text the team shows or the daemon writes for it follows that language:
// the desktop team page, Updates, the daemon's decisions and its messages to the leader.
// The translations are Qt Linguist files (modules/remote/translations), built into the
// binaries; English is the source text and needs none.
//
// The desktop keeps its tr() calls: a team page opens a Scope with its team's language while
// it builds, and the translator install() adds answers only inside a Scope. The daemon has a
// team at hand and asks text() for that team's language.

#include <QString>

namespace ketplus::remote::team_language {

inline const QString english = QStringLiteral("en");
inline const QString vietnamese = QStringLiteral("vi");

// The languages a team may have, in the order a picker offers them.
[[nodiscard]] QStringList supported();
[[nodiscard]] bool isSupported(const QString& language);
// A team without a language, such as one saved before T-56, is English.
[[nodiscard]] QString orEnglish(const QString& language);
// The supported language a BCP-47 tag or a locale name (vi, vi-VN, vi_VN) is, or English.
[[nodiscard]] QString fromLocale(const QString& tag);
// The language a person's text is written in, when its letters tell: "vi" for Vietnamese
// letters (ă, đ, ơ, ư, a tone mark), else empty.
[[nodiscard]] QString detect(const QString& text);
// The language's own name, as a picker shows it.
[[nodiscard]] QString nativeName(const QString& language);

// `source` in `language`: the translation, or the source when there is none.
[[nodiscard]] QString text(const QString& language, const char* context, const char* source,
                           int n = -1);

// Adds the team translator to the application, once; the first translated Scope calls it.
// Translations then apply to tr() calls made inside a Scope of a translated language, and
// nowhere else.
void install();

// Inside its lifetime, on this thread, tr() answers in `language`. Scopes nest.
class Scope final {
  public:
    explicit Scope(const QString& language);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

  private:
    QString previous_;
};

// The language of the team page the user has in front of them, or empty when none is shown.
// On the GUI thread, tr() outside a Scope answers in it, so a menu or a dialog opened from a
// team page, and the texts it changes later, speak the team's language too. Only the team
// contexts are translated, so the rest of the application is unaffected.
void setAmbient(const QString& language);

// The language of the innermost Scope on this thread, else the ambient one on the GUI thread;
// English without either.
[[nodiscard]] QString current();

} // namespace ketplus::remote::team_language

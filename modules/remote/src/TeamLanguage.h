#pragma once

// Team content preferences are independent of the application's English UI. Persisted
// en/vi values remain compatible content hints; auto follows the user's prompt and explicit
// content instructions. Catalogs remain available as historical resources, never installed.

#include <QString>

class QObject;

namespace ketplus::remote::team_language {

inline const QString automatic = QStringLiteral("auto");
inline const QString english = QStringLiteral("en");
inline const QString vietnamese = QStringLiteral("vi");

// The languages a team may have, in the order a picker offers them.
[[nodiscard]] QStringList supported();
[[nodiscard]] bool isSupported(const QString& language);
// Legacy normalization helper: unsupported/empty values become English. Creation and
// persistence explicitly default a missing preference to auto.
[[nodiscard]] QString orEnglish(const QString& language);
// Legacy utilities only; neither OS locale nor letter detection selects UI/content language.
// The supported language a BCP-47 tag or locale name represents, or English.
[[nodiscard]] QString fromLocale(const QString& tag);
// The language a person's text is written in, when its letters tell: "vi" for Vietnamese
// letters (ă, đ, ơ, ư, a tone mark), else empty.
[[nodiscard]] QString detect(const QString& text);
// English labels for the content-language picker (including Follow prompt).
[[nodiscard]] QString nativeName(const QString& language);

// English fixed system text, independent of content preferences. Does not touch stored text.
[[nodiscard]] QString text(const QString& language, const char* context, const char* source,
                           int n = -1);

// Compatibility no-ops: no translator is installed, and current() always returns English.
void install();
class Scope final {
  public:
    explicit Scope(const QString& language);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};
void setAmbient(const QString& language, QObject* owner = nullptr);
void clearAmbient(QObject* owner);
[[nodiscard]] QString current();

} // namespace ketplus::remote::team_language

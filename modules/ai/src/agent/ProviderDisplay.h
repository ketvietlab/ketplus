#pragma once

#include <QList>
#include <QString>

namespace ketplus::provider_display {

// The one table of the providers KetPlus offers, in the order every picker lists them.
// Provider ids remain protocol values; every visible name uses the product's spelling.
struct Provider final {
    const char* id;
    // Who makes it, and the product's own name, for a choice ("OpenAI – Codex").
    const char* company;
    const char* product;
    // The short name captions, banners and transcripts use ("GitHub Copilot").
    const char* name;
};

inline const QList<Provider>& providers() {
    static const QList<Provider> table{
        {"codex", "OpenAI", "Codex", "Codex"},
        {"claude", "Anthropic", "Claude", "Claude"},
        {"cursor", "Anysphere", "Cursor", "Cursor"},
        {"copilot", "GitHub", "Copilot", "GitHub Copilot"},
        {"ketrouter", "KétViệt", "KétRouter", "KétRouter"},
    };
    return table;
}

inline const Provider* find(const QString& provider) {
    for (const Provider& entry : providers())
        if (provider == QLatin1String(entry.id))
            return &entry;
    return nullptr;
}

inline QString name(const QString& provider) {
    if (const Provider* entry = find(provider))
        return QString::fromUtf8(entry->name);
    return provider.left(1).toUpper() + provider.mid(1);
}

// A provider in a select box: the company first, as "Anthropic – Claude".
inline QString choice(const QString& provider) {
    if (const Provider* entry = find(provider))
        return QString::fromUtf8(entry->company) + QStringLiteral(" – ") +
               QString::fromUtf8(entry->product);
    return name(provider);
}

} // namespace ketplus::provider_display

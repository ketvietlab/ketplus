#pragma once

#include "editor/EditorSettings.h"

#include <QString>

class QSettings;

namespace ketplus {

struct TextTypographySettings final {
    static constexpr int minimumFontSizePixels = 8;
    static constexpr int maximumFontSizePixels = 48;
    static constexpr int minimumLineHeightPixels = 12;
    static constexpr int maximumLineHeightPixels = 96;

    int fontSizePixels{14};
    int lineHeightPixels{22};

    [[nodiscard]] TextTypographySettings normalized() const;

    bool operator==(const TextTypographySettings&) const = default;
};

struct AppearanceSettings final {
    static constexpr int minimumInterfaceFontSizePixels = 8;
    static constexpr int maximumInterfaceFontSizePixels = 24;
    // Markdown reads one pixel larger than the interface, at the former 15/26 rhythm.
    static constexpr int markdownOffsetPixels = 1;
    static constexpr double markdownLineHeightRatio = 26.0 / 15.0;

    int interfaceFontSizePixels{14};
    EditorSettings editor{EditorSettings::defaults()};
    // Derived by normalized(): the editor's size and line height.
    TextTypographySettings terminal{14, 24};
    // Empty uses the KétPlus profile's bundled monospace family.
    QString terminalFontFamily;
    // Derived by normalized(): the interface size plus markdownOffsetPixels.
    TextTypographySettings preview{15, 26};

    [[nodiscard]] static AppearanceSettings defaults();
    [[nodiscard]] static AppearanceSettings load();
    [[nodiscard]] static AppearanceSettings load(const QSettings& settings);
    void save() const;
    void save(QSettings& settings) const;

    [[nodiscard]] AppearanceSettings normalized() const;

    bool operator==(const AppearanceSettings&) const = default;
};

} // namespace ketplus

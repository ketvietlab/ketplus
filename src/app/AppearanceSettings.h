#pragma once

#include "editor/EditorSettings.h"

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

    int interfaceFontSizePixels{14};
    EditorSettings editor{EditorSettings::defaults()};
    TextTypographySettings terminal{13, 20};
    TextTypographySettings preview{14, 22};

    [[nodiscard]] static AppearanceSettings defaults();
    [[nodiscard]] static AppearanceSettings load();
    [[nodiscard]] static AppearanceSettings load(const QSettings& settings);
    void save() const;
    void save(QSettings& settings) const;

    [[nodiscard]] AppearanceSettings normalized() const;

    bool operator==(const AppearanceSettings&) const = default;
};

} // namespace ketplus

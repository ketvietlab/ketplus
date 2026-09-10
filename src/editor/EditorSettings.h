#pragma once

#include <QFont>
#include <QString>

class QSettings;

namespace ketplus {

struct EditorSettings final {
    static constexpr int minimumFontSizePixels = 8;
    static constexpr int maximumFontSizePixels = 48;
    static constexpr int minimumLineHeightPixels = 12;
    static constexpr int maximumLineHeightPixels = 96;

    QString fontFamily;
    int fontSizePixels{13};
    int lineHeightPixels{24};

    [[nodiscard]] static EditorSettings defaults();
    [[nodiscard]] static EditorSettings load();
    [[nodiscard]] static EditorSettings load(const QSettings& settings);
    void save() const;
    void save(QSettings& settings) const;

    [[nodiscard]] EditorSettings normalized() const;
    [[nodiscard]] QFont font() const;

    bool operator==(const EditorSettings&) const = default;
};

} // namespace ketplus

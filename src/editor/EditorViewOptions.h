#pragma once

class QSettings;

namespace ketplus {

struct EditorViewOptions final {
    static constexpr int minimumTabWidth = 1;
    static constexpr int maximumTabWidth = 16;
    static constexpr int minimumRulerColumn = 1;
    static constexpr int maximumRulerColumn = 400;

    bool wordWrap{false};
    bool showWhitespace{false};
    bool indentGuides{true};
    bool showRuler{false};
    int rulerColumn{100};
    bool codeFolding{true};
    bool autoCloseBrackets{true};
    bool autoIndent{true};
    bool useTabs{false};
    int tabWidth{4};

    [[nodiscard]] static EditorViewOptions load();
    [[nodiscard]] static EditorViewOptions load(const QSettings& settings);
    void save() const;
    void save(QSettings& settings) const;

    [[nodiscard]] EditorViewOptions normalized() const;

    bool operator==(const EditorViewOptions&) const = default;
};

} // namespace ketplus

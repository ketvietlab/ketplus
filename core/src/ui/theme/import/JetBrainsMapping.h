#pragma once

namespace ketplus::jetbrains_detail {
struct ColorMapping {
    const char* source;
    const char* target;
};
inline constexpr ColorMapping editorColors[] = {
    {"CARET_COLOR", "editor.caret"},
    {"CARET_ROW_COLOR", "editor.lineHighlight"},
    {"SELECTION_BACKGROUND", "editor.selection"},
    {"SELECTION_FOREGROUND", "editor.selectionFg"},
    {"GUTTER_BACKGROUND", "editor.gutterBg"},
    {"LINE_NUMBERS_COLOR", "editor.gutterFg"},
    {"CONSOLE_BACKGROUND_KEY", "terminal.bg"},
    {"CONSOLE_BACKGROUND", "terminal.bg"},
};
inline constexpr ColorMapping attributeColors[] = {
    {"TEXT.FOREGROUND", "editor.fg"},
    {"TEXT.BACKGROUND", "editor.bg"},
    {"DEFAULT_TEXT.FOREGROUND", "editor.fg"},
    {"DEFAULT_TEXT.BACKGROUND", "editor.bg"},
    {"CONSOLE_NORMAL_OUTPUT.FOREGROUND", "terminal.fg"},
    {"CONSOLE_NORMAL_OUTPUT.BACKGROUND", "terminal.bg"},
    {"CONSOLE_ERROR_OUTPUT.FOREGROUND", "terminal.ansi1"},
    {"CONSOLE_SYSTEM_OUTPUT.FOREGROUND", "terminal.ansi8"},
    {"CONSOLE_BLACK_OUTPUT.FOREGROUND", "terminal.ansi0"},
    {"CONSOLE_RED_OUTPUT.FOREGROUND", "terminal.ansi1"},
    {"CONSOLE_GREEN_OUTPUT.FOREGROUND", "terminal.ansi2"},
    {"CONSOLE_YELLOW_OUTPUT.FOREGROUND", "terminal.ansi3"},
    {"CONSOLE_BLUE_OUTPUT.FOREGROUND", "terminal.ansi4"},
    {"CONSOLE_MAGENTA_OUTPUT.FOREGROUND", "terminal.ansi5"},
    {"CONSOLE_CYAN_OUTPUT.FOREGROUND", "terminal.ansi6"},
    {"CONSOLE_GRAY_OUTPUT.FOREGROUND", "terminal.ansi7"},
    {"CONSOLE_DARKGRAY_OUTPUT.FOREGROUND", "terminal.ansi8"},
    {"CONSOLE_RED_BRIGHT_OUTPUT.FOREGROUND", "terminal.ansi9"},
    {"CONSOLE_GREEN_BRIGHT_OUTPUT.FOREGROUND", "terminal.ansi10"},
    {"CONSOLE_YELLOW_BRIGHT_OUTPUT.FOREGROUND", "terminal.ansi11"},
    {"CONSOLE_BLUE_BRIGHT_OUTPUT.FOREGROUND", "terminal.ansi12"},
    {"CONSOLE_MAGENTA_BRIGHT_OUTPUT.FOREGROUND", "terminal.ansi13"},
    {"CONSOLE_CYAN_BRIGHT_OUTPUT.FOREGROUND", "terminal.ansi14"},
    {"CONSOLE_WHITE_OUTPUT.FOREGROUND", "terminal.ansi15"},
    {"DIFF_INSERTED.BACKGROUND", "diff.added"},
    {"DIFF_DELETED.BACKGROUND", "diff.removed"},
};
struct SyntaxMapping {
    const char* source;
    const char* role;
    bool languageSpecific = false;
    // One canonical DEFAULT_* key owns each role; other keys are fallbacks only.
    bool primary = true;
};
inline constexpr SyntaxMapping syntaxMappings[] = {
    {"DEFAULT_KEYWORD", "keyword"},
    {"DEFAULT_STRING", "string"},
    {"DEFAULT_NUMBER", "number"},
    {"DEFAULT_CLASS_NAME", "type"},
    {"DEFAULT_CLASS_REFERENCE", "type", false, false},
    {"DEFAULT_INTERFACE_NAME", "type", false, false},
    {"DEFAULT_LINE_COMMENT", "comment"},
    {"DEFAULT_BLOCK_COMMENT", "comment", false, false},
    {"DEFAULT_DOC_COMMENT", "comment", false, false},
    {"DEFAULT_METADATA", "attribute"},
    {"DEFAULT_FUNCTION_DECLARATION", "function"},
    {"DEFAULT_FUNCTION_CALL", "function", false, false},
    {"DEFAULT_INSTANCE_METHOD", "function", false, false},
    {"DEFAULT_STATIC_METHOD", "function", false, false},
    {"DEFAULT_LOCAL_VARIABLE", "variable"},
    {"DEFAULT_GLOBAL_VARIABLE", "variable", false, false},
    {"DEFAULT_PARAMETER", "variable", false, false},
    {"DEFAULT_INSTANCE_FIELD", "variable", false, false},
    {"DEFAULT_STATIC_FIELD", "variable", false, false},
    {"DEFAULT_CONSTANT", "constant"},
    {"DEFAULT_OPERATION_SIGN", "operator"},
    {"JAVA_KEYWORD", "keyword", true, false},
    {"JAVA_STRING", "string", true, false},
    {"JAVA_NUMBER", "number", true, false},
    {"JAVA_CLASS", "type", true, false},
    {"JAVA_METHOD_DECLARATION", "function", true, false},
    {"KOTLIN_KEYWORD", "keyword", true, false},
    {"KOTLIN_STRING", "string", true, false},
    {"KOTLIN_NUMBER", "number", true, false},
    {"KOTLIN_CLASS", "type", true, false},
    {"KOTLIN_FUNCTION_DECLARATION", "function", true, false},
};
inline constexpr ColorMapping uiColors[] = {
    {"Panel.background", "ui.chrome.panelBackground"},
    {"Panel.foreground", "ui.chrome.textMain"},
    {"Label.foreground", "ui.chrome.textMain"},
    {"Label.disabledForeground", "ui.chrome.textDisabled"},
    {"ToolWindow.background", "ui.sidebar.bg"},
    {"ToolWindow.foreground", "ui.sidebar.fg"},
    {"TextField.background", "ui.input.bg"},
    {"TextField.foreground", "ui.input.fg"},
    {"TextField.borderColor", "ui.input.border"},
    {"Component.borderColor", "ui.chrome.border"},
    {"Component.focusColor", "ui.focus.border"},
    {"Component.accentColor", "ui.chrome.accent"},
    {"List.background", "ui.chrome.pageBackground"},
    {"List.foreground", "ui.list.fg"},
    {"List.selectionBackground", "ui.list.selection"},
    {"List.selectionForeground", "ui.list.selectionFg"},
    {"List.hoverBackground", "ui.list.hover"},
    {"Tree.background", "ui.sidebar.bg"},
    {"Tree.foreground", "ui.sidebar.fg"},
    {"Tree.selectionBackground", "ui.list.selection"},
    {"Tree.selectionForeground", "ui.list.selectionFg"},
    {"EditorTabs.selectedBackground", "ui.tab.activeBg"},
    {"EditorTabs.selectedForeground", "ui.tab.activeFg"},
    {"EditorTabs.background", "ui.tab.inactiveBg"},
    {"EditorTabs.foreground", "ui.tab.inactiveFg"},
    {"Button.background", "ui.button.bg"},
    {"Button.foreground", "ui.button.fg"},
    {"Button.hoverBackground", "ui.button.hover"},
    {"Button.pressedBackground", "ui.button.active"},
};
} // namespace ketplus::jetbrains_detail

#pragma once

#include "KetControlRecipes.h"
#include "KetSurfaceRecipes.h"

namespace ketplus::tree_rows {

// Where a tree row puts its parts, in pixels from the row's left edge. `px` turns a kit length
// into device-independent pixels at the interface zoom. A file and a folder at one depth share
// the icon and the name columns: the file keeps the width of the folder's chevron free, so
// nothing about a row's kind moves what follows it.
struct ExplorerColumns final {
    int chevron = 0;
    int icon = 0;
    int text = 0;
};

template <typename Scale>
[[nodiscard]] ExplorerColumns explorerColumns(const ket::FileRowRecipe& recipe, const int depth,
                                              Scale&& px) {
    const int gap = px(recipe.folderSpacing);
    ExplorerColumns columns;
    columns.chevron = px(recipe.indentStart + depth * recipe.indentStep);
    columns.icon = columns.chevron + px(recipe.chevronSize) + gap;
    columns.text = columns.icon + px(recipe.iconSize) + gap;
    return columns;
}

// The Git changes tree has two levels: a group header (chevron, then its title) and the files
// under it. A file's icon sits under the title of its group, the width of the chevron in.
struct SourceControlColumns final {
    int groupChevron = 0;
    int groupText = 0;
    int fileIcon = 0;
    int fileText = 0;
};

template <typename Scale>
[[nodiscard]] SourceControlColumns sourceControlColumns(const ket::FileRowRecipe& file,
                                                        const ket::GroupHeaderRecipe& group,
                                                        Scale&& px) {
    SourceControlColumns columns;
    columns.groupChevron = px(file.paddingInline);
    columns.groupText = columns.groupChevron + px(group.iconSize) + px(file.spacing);
    columns.fileIcon = columns.groupText;
    columns.fileText = columns.fileIcon + px(file.iconSize) + px(file.spacing);
    return columns;
}

} // namespace ketplus::tree_rows

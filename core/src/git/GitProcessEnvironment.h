#pragma once

#include <QProcessEnvironment>

namespace ketplus {

inline QProcessEnvironment gitProcessEnvironment() {
    auto environment = QProcessEnvironment::systemEnvironment();
    // A launcher started by Git (for example a hook) can inherit overrides for a
    // different checkout. Let the selected directory determine repository identity.
    for (const auto* variable :
         {"GIT_ALTERNATE_OBJECT_DIRECTORIES", "GIT_CONFIG", "GIT_CONFIG_PARAMETERS",
          "GIT_CONFIG_COUNT", "GIT_OBJECT_DIRECTORY", "GIT_DIR", "GIT_WORK_TREE",
          "GIT_IMPLICIT_WORK_TREE", "GIT_GRAFT_FILE", "GIT_INDEX_FILE", "GIT_NO_REPLACE_OBJECTS",
          "GIT_REPLACE_REF_BASE", "GIT_PREFIX", "GIT_SHALLOW_FILE", "GIT_COMMON_DIR",
          "GIT_LITERAL_PATHSPECS", "GIT_GLOB_PATHSPECS", "GIT_NOGLOB_PATHSPECS",
          "GIT_ICASE_PATHSPECS"}) {
        environment.remove(QLatin1StringView(variable));
    }
    environment.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    environment.insert(QStringLiteral("GIT_PAGER"), QStringLiteral("cat"));
    environment.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    return environment;
}

} // namespace ketplus

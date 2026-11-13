set(daemon "${KETPLUS_APP_BUNDLE}/Contents/Helpers/ketplus_daemon")
if(NOT EXISTS "${daemon}")
    message(FATAL_ERROR "KetPlus.app does not contain Contents/Helpers/ketplus_daemon")
endif()

execute_process(
    COMMAND "${daemon}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT version_result EQUAL 0)
    message(FATAL_ERROR "Bundled daemon is not executable: ${version_error}")
endif()
if(NOT version_output STREQUAL "ketplus-daemon ${KETPLUS_PRODUCT_VERSION}")
    message(FATAL_ERROR
        "Desktop/daemon version mismatch: expected ${KETPLUS_PRODUCT_VERSION}, got ${version_output}"
    )
endif()

execute_process(
    COMMAND "${daemon}" --check-runtime
    RESULT_VARIABLE runtime_result
    OUTPUT_VARIABLE runtime_output
    ERROR_VARIABLE runtime_error
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT runtime_result EQUAL 0)
    message(FATAL_ERROR
        "Bundled daemon cannot load its required Qt plugins: ${runtime_error}"
    )
endif()
if(NOT runtime_output STREQUAL "ketplus-daemon runtime ok")
    message(FATAL_ERROR "Unexpected bundled daemon runtime check: ${runtime_output}")
endif()

# A Release daemon cannot be pointed elsewhere at run time, so the relay compiled into the
# bundle must be the one this build was configured for.
if(KETPLUS_PRODUCTION_RELEASE OR (DEFINED KETPLUS_CONTROL_URL AND NOT KETPLUS_CONTROL_URL STREQUAL ""))
    execute_process(
        COMMAND "${daemon}" --build-info
        RESULT_VARIABLE build_info_result
        OUTPUT_VARIABLE build_info
        ERROR_VARIABLE build_info_error
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(NOT build_info_result EQUAL 0)
        message(FATAL_ERROR "Bundled daemon cannot report its build: ${build_info_error}")
    endif()
    include("${CMAKE_CURRENT_LIST_DIR}/../../cmake/KetPlusProductionControl.cmake")
    ketplus_verify_control_build_info("${build_info}" "${KETPLUS_CONTROL_URL}"
        "${KETPLUS_PRODUCTION_RELEASE}")
endif()

execute_process(
    COMMAND /usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString"
        "${KETPLUS_APP_BUNDLE}/Contents/Info.plist"
    RESULT_VARIABLE app_version_result
    OUTPUT_VARIABLE app_version
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
if(NOT app_version_result EQUAL 0 OR
   NOT "${app_version}" STREQUAL "${KETPLUS_PRODUCT_VERSION}")
    message(FATAL_ERROR
        "Desktop bundle version mismatch: expected ${KETPLUS_PRODUCT_VERSION}, got ${app_version}"
    )
endif()

# ADR 0019: a build configured for updates must carry Sparkle and the exact feed and key it
# was configured with; an unconfigured build must not carry either.
set(sparkle_framework "${KETPLUS_APP_BUNDLE}/Contents/Frameworks/Sparkle.framework")
foreach(sparkle_key IN ITEMS SUFeedURL SUPublicEDKey)
    execute_process(
        COMMAND /usr/libexec/PlistBuddy -c "Print :${sparkle_key}"
            "${KETPLUS_APP_BUNDLE}/Contents/Info.plist"
        RESULT_VARIABLE sparkle_key_result
        OUTPUT_VARIABLE sparkle_key_value
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    set(sparkle_plist_${sparkle_key} "")
    if(sparkle_key_result EQUAL 0)
        set(sparkle_plist_${sparkle_key} "${sparkle_key_value}")
    endif()
endforeach()
if(KETPLUS_UPDATES_ENABLED)
    if(NOT EXISTS "${sparkle_framework}/Versions/B/Sparkle"
       OR NOT IS_SYMLINK "${sparkle_framework}/Sparkle")
        message(FATAL_ERROR "KetPlus.app is missing an intact Contents/Frameworks/Sparkle.framework")
    endif()
    if(NOT sparkle_plist_SUFeedURL STREQUAL KETPLUS_UPDATE_FEED_URL
       OR NOT sparkle_plist_SUPublicEDKey STREQUAL KETPLUS_UPDATE_PUBLIC_ED_KEY)
        message(FATAL_ERROR "KetPlus.app Info.plist does not carry the configured update feed and key")
    endif()
elseif(DEFINED KETPLUS_UPDATES_ENABLED)
    if(EXISTS "${sparkle_framework}" OR NOT sparkle_plist_SUFeedURL STREQUAL "")
        message(FATAL_ERROR "KetPlus.app carries Sparkle although updates are not configured")
    endif()
endif()

file(GLOB_RECURSE bundled_codex LIST_DIRECTORIES false
    "${KETPLUS_APP_BUNDLE}/*codex*"
    "${KETPLUS_APP_BUNDLE}/*Codex*"
)
if(bundled_codex)
    message(FATAL_ERROR "Codex must remain external and was found in the app bundle: ${bundled_codex}")
endif()

if(EXISTS "${KETPLUS_APP_BUNDLE}/Contents/Helpers/claude-sidecar")
    message(FATAL_ERROR
        "Claude sidecar must live in Contents/Resources, not Contents/Helpers")
endif()
# Without the sidecar every Claude session fails with "Claude Agent SDK sidecar
# was not found", so a bundle without a complete one is never shippable.
foreach(sidecar_required IN ITEMS
        index.mjs transcript.mjs input.mjs runtime.json
        node_modules/@anthropic-ai/claude-agent-sdk/package.json)
    if(NOT EXISTS "${KETPLUS_APP_BUNDLE}/Contents/Resources/claude-sidecar/${sidecar_required}")
        message(FATAL_ERROR
            "KetPlus.app is missing Contents/Resources/claude-sidecar/${sidecar_required}; "
            "stage it with apps/daemon/providers/claude-sidecar/package-sidecar.mjs")
    endif()
endforeach()

# Agents are told to read these default skills; a missing one silently drops it.
foreach(agent_skill IN ITEMS ket-atlas enterprise-grade ket-design-system)
    if(NOT EXISTS "${KETPLUS_APP_BUNDLE}/Contents/Resources/agent-skills/${agent_skill}/SKILL.md")
        message(FATAL_ERROR
            "KetPlus.app is missing Contents/Resources/agent-skills/${agent_skill}/SKILL.md")
    endif()
endforeach()

file(GLOB_RECURSE sidecar_entries LIST_DIRECTORIES true
    "${KETPLUS_APP_BUNDLE}/Contents/Resources/claude-sidecar/*")
foreach(sidecar_entry IN LISTS sidecar_entries)
    if(IS_SYMLINK "${sidecar_entry}")
        file(READ_SYMLINK "${sidecar_entry}" sidecar_link)
        if(IS_ABSOLUTE "${sidecar_link}" OR NOT EXISTS "${sidecar_entry}")
            message(FATAL_ERROR "Claude sidecar contains a non-relocatable link: ${sidecar_entry}")
        endif()
    endif()
endforeach()

if(KETPLUS_REQUIRE_DEPLOYED_RUNTIME)
    set(required_runtime_paths
        "Contents/Frameworks/QtCore.framework/QtCore"
        "Contents/Frameworks/QtSql.framework/QtSql"
        "Contents/PlugIns/sqldrivers/libqsqlite.dylib"
        "Contents/PlugIns/libqtquickcontrols2plugin.dylib"
        "Contents/Resources/qml/QtQuick/Controls/qmldir"
        "Contents/Resources/qml/QtQuick/Controls/libqtquickcontrols2plugin.dylib"
    )
    foreach(relative_path IN LISTS required_runtime_paths)
        if(NOT EXISTS "${KETPLUS_APP_BUNDLE}/${relative_path}")
            message(FATAL_ERROR "KetPlus.app is missing deployed runtime: ${relative_path}")
        endif()
    endforeach()

    if(IS_SYMLINK "${KETPLUS_APP_BUNDLE}/Contents/PlugIns/libqtquickcontrols2plugin.dylib")
        message(FATAL_ERROR "Deployed QML plugin must not point outside KetPlus.app")
    endif()

    execute_process(
        COMMAND /usr/bin/otool -L
            "${KETPLUS_APP_BUNDLE}/Contents/MacOS/KetPlus"
            "${daemon}"
            "${KETPLUS_APP_BUNDLE}/Contents/Resources/qml/QtQuick/Controls/libqtquickcontrols2plugin.dylib"
        RESULT_VARIABLE otool_result
        OUTPUT_VARIABLE dependency_output
        ERROR_VARIABLE otool_error
    )
    if(NOT otool_result EQUAL 0)
        message(FATAL_ERROR "Unable to inspect packaged dependencies: ${otool_error}")
    endif()
    if(dependency_output MATCHES "/(opt/homebrew|usr/local)/")
        message(FATAL_ERROR "KetPlus.app contains build-machine dependencies: ${dependency_output}")
    endif()

    execute_process(
        COMMAND /usr/bin/codesign --verify --deep --strict "${KETPLUS_APP_BUNDLE}"
        RESULT_VARIABLE codesign_result
        ERROR_VARIABLE codesign_error
    )
    if(NOT codesign_result EQUAL 0)
        message(FATAL_ERROR "KetPlus.app has an invalid code signature: ${codesign_error}")
    endif()
    # --deep does not reach Mach-O files under Contents/Resources, and the Claude
    # sidecar ships the native claude CLI and addons there.
    if(EXISTS "${KETPLUS_APP_BUNDLE}/Contents/Resources/claude-sidecar")
        file(GLOB_RECURSE sidecar_files LIST_DIRECTORIES false
            "${KETPLUS_APP_BUNDLE}/Contents/Resources/claude-sidecar/*")
        foreach(sidecar_file IN LISTS sidecar_files)
            file(READ "${sidecar_file}" sidecar_magic LIMIT 4 HEX)
            if(sidecar_magic MATCHES "^(cffaedfe|cefaedfe|cafebabe|feedfacf|feedface)$")
                execute_process(
                    COMMAND /usr/bin/codesign --verify --strict "${sidecar_file}"
                    RESULT_VARIABLE sidecar_codesign_result
                    ERROR_VARIABLE sidecar_codesign_error
                )
                if(NOT sidecar_codesign_result EQUAL 0)
                    message(FATAL_ERROR
                        "Claude sidecar binary is not signed: ${sidecar_file}: ${sidecar_codesign_error}")
                endif()
            endif()
        endforeach()
    endif()
endif()

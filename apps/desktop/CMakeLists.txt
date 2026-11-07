set(ketplus_pairing_qr_source src/remote/PairingQrCode.cpp)
if(APPLE)
    set(ketplus_pairing_qr_source src/remote/PairingQrCode.mm)
endif()

add_library(ketplus_desktop_support STATIC
    src/remote/PairingQrCode.h
    ${ketplus_pairing_qr_source}
    src/remote/RemoteControlView.cpp
    src/remote/RemoteControlView.h
    src/remote/LinuxMachinesSection.cpp
    src/remote/LinuxMachinesSection.h
    src/remote/RemoteActivitySection.cpp
    src/remote/RemoteActivitySection.h
    src/remote/RemoteConnectionsIntro.cpp
    src/remote/RemoteConnectionsIntro.h
    src/app/RepositoryMetadataStore.cpp
    src/app/RepositoryMetadataStore.h
    src/app/RepositoryRouter.cpp
    src/app/RepositoryRouter.h
    src/app/SpotlightDialog.cpp
    src/app/SpotlightDialog.h
    src/workspace/ActivityBar.cpp
    src/workspace/ActivityBar.h
    src/workspace/LucideIcons.h
    src/workspace/SidebarRepositoryList.cpp
    src/workspace/SidebarRepositoryList.h
)
target_link_libraries(ketplus_desktop_support PUBLIC
    Qt6::Network
    Qt6::Sql
    Qt6::Svg
    Qt6::Widgets
    ketplus_remote
    ketplus_remote_control
    ketplus_typography
    KetPlusCM::git
    KetPlusCM::ui
    KetPlusCM::workspace
)
if(APPLE)
    target_link_libraries(ketplus_desktop_support PUBLIC "-framework CoreImage")
endif()
target_include_directories(ketplus_desktop_support PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_compile_definitions(ketplus_desktop_support PUBLIC
    $<$<AND:$<BOOL:${KETPLUS_ENABLE_REMOTE_DEMO}>,$<CONFIG:Debug>>:KETPLUS_REMOTE_DEMO_AVAILABLE=1>
)
ketplus_enable_warnings(ketplus_desktop_support)
# Connections before sign-in shows the Qt mock's captures, kept in one place for both.
qt_add_resources(ketplus_desktop_support "ketplus_connections_intro"
    PREFIX "/connections"
    BASE "${PROJECT_SOURCE_DIR}/mockups/qt/assets/connections"
    FILES
        "${PROJECT_SOURCE_DIR}/mockups/qt/assets/connections/desktop-question-dark.png"
        "${PROJECT_SOURCE_DIR}/mockups/qt/assets/connections/desktop-question-light.png"
        "${PROJECT_SOURCE_DIR}/mockups/qt/assets/connections/mobile-question-dark.png"
        "${PROJECT_SOURCE_DIR}/mockups/qt/assets/connections/mobile-question-light.png"
)

# ADR 0019: the process-wide in-app updater. Without a configured feed, or off macOS, it is a
# disabled implementation that never contacts a server.
add_library(ketplus_app_updater STATIC
    src/update/AppUpdater.cpp
    src/update/AppUpdater.h
)
target_link_libraries(ketplus_app_updater PUBLIC Qt6::Core)
target_include_directories(ketplus_app_updater PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
if(KETPLUS_UPDATES_ENABLED)
    target_sources(ketplus_app_updater PRIVATE src/update/SparkleAppUpdater.mm)
    set_source_files_properties(src/update/SparkleAppUpdater.mm
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_compile_definitions(ketplus_app_updater PRIVATE KETPLUS_HAS_SPARKLE=1)
    target_link_libraries(ketplus_app_updater PUBLIC ketplus_sparkle "-framework Foundation")
endif()
ketplus_enable_warnings(ketplus_app_updater)

add_library(ketplus_desktop_shell STATIC
    src/app/ArchivedSettingsPage.cpp
    src/app/ArchivedSettingsPage.h
    src/app/AgentWorktreeCreator.cpp
    src/app/AgentWorktreeCreator.h
    src/app/AtlasBuilderTab.cpp
    src/app/AtlasBuilderTab.h
    src/app/AtlasConversationPanel.cpp
    src/app/AtlasConversationPanel.h
    src/app/ExecutionWorktreeDialog.cpp
    src/app/ExecutionWorktreeDialog.h
    src/app/AgentWorktreeDialog.cpp
    src/app/AgentWorktreeDialog.h
    src/app/MainWindowAppearance.cpp
    src/app/MainWindow.cpp
    src/app/MainWindow.h
    src/app/MainWindowActions.cpp
    src/app/MainWindowAgent.cpp
    src/app/MainWindowTranscriptLinks.cpp
    src/app/TranscriptFileType.cpp
    src/app/TranscriptFileType.h
    src/app/MainWindowAtlas.cpp
    src/app/MainWindowDefinition.cpp
    src/app/MainWindowDocuments.cpp
    src/app/MainWindowLayout.cpp
    src/app/MainWindowNavigation.cpp
    src/app/MainWindowPanels.cpp
    src/app/MainWindowRemote.cpp
    src/app/MainWindowSidebar.cpp
    src/app/MainWindowSearch.cpp
    src/app/MainWindowWorkspace.cpp
    src/app/MainWindowWorktrees.cpp
    src/app/SettingsBrowserPage.cpp
    src/app/SettingsBrowserPage.h
    src/app/SettingsClaudeAnalytics.cpp
    src/app/SettingsClaudeAnalytics.h
    src/app/SettingsClaudeCredentials.cpp
    src/app/SettingsClaudeCredentials.h
    src/app/SettingsComputerPage.cpp
    src/app/SettingsComputerPage.h
    src/app/SettingsStoragePage.cpp
    src/app/SettingsStoragePage.h
    src/app/SettingsProvidersPage.cpp
    src/app/SettingsProvidersPage.h
    src/app/SettingsUpdatesPage.cpp
    src/app/SettingsUpdatesPage.h
    src/app/AgentConversationWidth.cpp
    src/app/AgentConversationWidth.h
    src/app/SettingsWorkspacePage.cpp
    src/app/SettingsWorkspacePage.h
    src/app/WorktreesWorkspacePage.cpp
    src/app/WorktreesWorkspacePage.h
)
target_link_libraries(ketplus_desktop_shell PUBLIC
    Qt6::Concurrent
    Qt6::Network
    Qt6::Widgets
    ketplus_ai
    ketplus_app_updater
    ketplus_computer_tools
    ketplus_atlas
    ketplus_desktop_support
    KetPlusCM::core
    KetPlusCM::editor
    KetPlusCM::git
    KetPlusCM::git_ui
    KetPlusCM::index
    KetPlusCM::preview
    KetPlusCM::settings_ui
    KetPlusCM::terminal
    KetPlusCM::ui
    KetPlusCM::workspace
)
target_include_directories(ketplus_desktop_shell PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
ketplus_enable_warnings(ketplus_desktop_shell)

set(ketplus_platform_sources)
if(APPLE)
    set(ketplus_macos_icon "${PROJECT_SOURCE_DIR}/assets/icons/KetPlus.icns")
    set_source_files_properties(
        "${ketplus_macos_icon}"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources"
    )
    list(APPEND ketplus_platform_sources "${ketplus_macos_icon}")
elseif(WIN32)
    list(APPEND ketplus_platform_sources "${PROJECT_SOURCE_DIR}/assets/icons/KetPlus.rc")
endif()

qt_add_executable(ketplus WIN32 MACOSX_BUNDLE
    src/main.cpp
    ${ketplus_platform_sources}
)
target_link_libraries(ketplus PRIVATE ketplus_desktop_shell ketplus_product_version)
ketplus_enable_warnings(ketplus)
ketplus_cm_add_font_resources(ketplus)

qt_add_resources(ketplus "ketplus_brand"
    PREFIX "/brand"
    BASE "${PROJECT_SOURCE_DIR}/assets/brand"
    FILES
        "${PROJECT_SOURCE_DIR}/assets/brand/ketplus-app-icon.png"
        "${PROJECT_SOURCE_DIR}/assets/brand/ketplus-mark.png"
)

qt_add_resources(ketplus "ketplus_wordmarks"
    PREFIX "/brand"
    BASE "${PROJECT_SOURCE_DIR}/ketplus.ketatlas/assets/brand/ketplus-v1"
    FILES
        "${PROJECT_SOURCE_DIR}/ketplus.ketatlas/assets/brand/ketplus-v1/ketplus-logo-light.png"
        "${PROJECT_SOURCE_DIR}/ketplus.ketatlas/assets/brand/ketplus-v1/ketplus-logo-dark.png"
)

set_target_properties(ketplus PROPERTIES
    OUTPUT_NAME "KetPlus"
    MACOSX_BUNDLE_BUNDLE_NAME "KétPlus Pro"
    MACOSX_BUNDLE_GUI_IDENTIFIER "dev.ketplus.editor"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
    MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
)

if(APPLE)
    set_target_properties(ketplus PROPERTIES MACOSX_BUNDLE_ICON_FILE "KetPlus.icns")

    # Sparkle reads its feed and public key from Info.plist. Automatic checks start on (users
    # turn them off in Settings → Updates), so Sparkle never asks on the second launch.
    set(KETPLUS_UPDATE_PLIST_KEYS "")
    if(KETPLUS_UPDATES_ENABLED)
        string(CONCAT KETPLUS_UPDATE_PLIST_KEYS
            "\t<key>SUFeedURL</key>\n\t<string>${KETPLUS_UPDATE_FEED_URL}</string>\n"
            "\t<key>SUPublicEDKey</key>\n\t<string>${KETPLUS_UPDATE_PUBLIC_ED_KEY}</string>\n"
            "\t<key>SUEnableAutomaticChecks</key>\n\t<true/>\n"
            "\t<key>SUVerifyUpdateBeforeExtraction</key>\n\t<true/>\n")
    endif()
    configure_file("${PROJECT_SOURCE_DIR}/packaging/macos/Info.plist.in"
        "${CMAKE_CURRENT_BINARY_DIR}/Info.plist.in" @ONLY)
    set_target_properties(ketplus PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist.in"
        INSTALL_RPATH "@executable_path/../Frameworks"
    )

    # The QML files live in ketplus_ai's resource collection rather than in a
    # qt_add_qml_module() owned by the executable. Recording them here gives
    # Qt's deploy scanner both rebuild dependencies and the import roots needed
    # to package the runtime modules used by Agent Workspace.
    file(GLOB ketplus_packaged_qml CONFIGURE_DEPENDS
        "${PROJECT_SOURCE_DIR}/modules/ai/src/agent/qml/*.qml"
    )
    set_property(TARGET ketplus PROPERTY QT_QML_MODULE_QML_FILES
        "${ketplus_packaged_qml}"
    )

    # Keep the per-user daemon inside the application bundle. This gives the
    # user one movable application and ensures Desktop always launches the
    # helper from the same product version before falling back to development
    # installations on PATH.
    #
    # ADR 0009: JavaScript trees must not live in Contents/Helpers (codesign
    # treats every file there as nested code). The Claude sidecar is JS +
    # node_modules in Resources; Node is reused from ketplus-browser. It is
    # installed after install(TARGETS ketplus), below.
    add_custom_target(ketplus_bundle_daemon ALL
        COMMAND "${CMAKE_COMMAND}" -E make_directory
            "$<TARGET_BUNDLE_DIR:ketplus>/Contents/Helpers"
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "$<TARGET_FILE:ketplus_daemon>"
            "$<TARGET_BUNDLE_DIR:ketplus>/Contents/Helpers/$<TARGET_FILE_NAME:ketplus_daemon>"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf
            "$<TARGET_BUNDLE_DIR:ketplus>/Contents/Resources/claude-sidecar"
        COMMAND /bin/cp -R "${KETPLUS_CLAUDE_SIDECAR_STAGE}"
            "$<TARGET_BUNDLE_DIR:ketplus>/Contents/Resources/claude-sidecar"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf
            "$<TARGET_BUNDLE_DIR:ketplus>/Contents/Resources/agent-skills"
        COMMAND "${CMAKE_COMMAND}" -E copy_directory
            "${PROJECT_SOURCE_DIR}/assets/agent-skills"
            "$<TARGET_BUNDLE_DIR:ketplus>/Contents/Resources/agent-skills"
        DEPENDS ketplus ketplus_daemon ketplus_stage_claude_sidecar
        VERBATIM
    )
    # ADR 0030: KetPlus Computer is a separately signed app nested in Contents/Helpers so TCC
    # attributes Screen Recording and Accessibility to it. TCC keys grants to the signature's
    # designated requirement: an ad-hoc identity resets them on every rebuild.
    option(KETPLUS_BUNDLE_COMPUTER_AGENT
        "Build KetPlus Computer.app (Swift) into KetPlus.app/Contents/Helpers" ON)
    set(KETPLUS_COMPUTER_SIGN_IDENTITY "-" CACHE STRING
        "codesign identity for KetPlus Computer.app; '-' signs ad hoc")
    if(KETPLUS_BUNDLE_COMPUTER_AGENT)
        add_custom_target(ketplus_bundle_computer_agent ALL
            COMMAND "${PROJECT_SOURCE_DIR}/apps/computer/scripts/build-app.sh"
                --identity "${KETPLUS_COMPUTER_SIGN_IDENTITY}"
                --out "$<TARGET_BUNDLE_DIR:ketplus>/Contents/Helpers"
                --scratch "${CMAKE_CURRENT_BINARY_DIR}/computer-swift"
            DEPENDS ketplus
            USES_TERMINAL
            VERBATIM
        )
        # ditto keeps the nested app's own signature.
        install(CODE "
            set(computer_destination \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/KetPlus.app/Contents/Helpers/KetPlus Computer.app\")
            file(REMOVE_RECURSE \"\${computer_destination}\")
            execute_process(
                COMMAND /usr/bin/ditto \"$<TARGET_BUNDLE_DIR:ketplus>/Contents/Helpers/KetPlus Computer.app\" \"\${computer_destination}\"
                RESULT_VARIABLE computer_copy_result)
            if(NOT computer_copy_result EQUAL 0)
                message(FATAL_ERROR \"Unable to embed KetPlus Computer.app\")
            endif()"
            COMPONENT Runtime
        )
    endif()
    # ADR 0009: the browser.v0 runtime (pinned Node + apps/browser production
    # dependencies + skill) lives in Contents/Resources/ketplus-browser. It is
    # not in Contents/Helpers because codesign treats every file there as nested
    # code and refuses the JavaScript tree. Staging
    # needs a build-host node (>= 22) and network for the pinned, sha256-checked
    # Node archive and npm packages, unless KETPLUS_BROWSER_RUNTIME_DIR points at
    # an already staged runtime (CI) or KETPLUS_NODE_CACHE holds the archive.
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        set(ketplus_bundle_browser_runtime_default ON)
    else()
        set(ketplus_bundle_browser_runtime_default OFF)
    endif()
    option(KETPLUS_BUNDLE_BROWSER_RUNTIME
        "Bundle the browser.v0 runtime in KetPlus.app/Contents/Resources/ketplus-browser"
        ${ketplus_bundle_browser_runtime_default})
    set(KETPLUS_BROWSER_RUNTIME_DIR "" CACHE PATH
        "Prebuilt staged browser runtime to bundle instead of running package-runtime.mjs")
    set(KETPLUS_NODE_CACHE "" CACHE PATH
        "Directory holding the pinned Node.js archive for package-runtime.mjs")

    if(KETPLUS_BUNDLE_BROWSER_RUNTIME)
        set(ketplus_browser_app_dir "${PROJECT_SOURCE_DIR}/apps/browser")
        message(STATUS
            "KETPLUS_BUNDLE_BROWSER_RUNTIME=ON: staging the browser runtime needs a build-host "
            "node >= 22 and network (nodejs.org, npm registry), unless KETPLUS_BROWSER_RUNTIME_DIR "
            "points at a staged runtime or KETPLUS_NODE_CACHE holds the pinned Node archive "
            "(npm packages still need network). Pass -DKETPLUS_BUNDLE_BROWSER_RUNTIME=OFF to skip.")
        file(READ "${ketplus_browser_app_dir}/runtime/node-runtime.json" ketplus_node_pin_json)
        string(JSON ketplus_node_pin_version GET "${ketplus_node_pin_json}" version)
        if(CMAKE_OSX_ARCHITECTURES)
            set(ketplus_browser_runtime_arch "${CMAKE_OSX_ARCHITECTURES}")
        else()
            set(ketplus_browser_runtime_arch "${CMAKE_SYSTEM_PROCESSOR}")
        endif()
        if(ketplus_browser_runtime_arch MATCHES "^(x86_64|AMD64)$")
            set(ketplus_browser_runtime_arch x64)
        elseif(ketplus_browser_runtime_arch MATCHES "^(arm64|aarch64)$")
            set(ketplus_browser_runtime_arch arm64)
        endif()
        set(ketplus_browser_runtime_platform "darwin-${ketplus_browser_runtime_arch}")
        include("${PROJECT_SOURCE_DIR}/cmake/KetPlusBrowserRuntime.cmake")
        if(KETPLUS_BROWSER_RUNTIME_DIR)
            if(NOT EXISTS "${KETPLUS_BROWSER_RUNTIME_DIR}/runtime.json")
                message(FATAL_ERROR
                    "KETPLUS_BROWSER_RUNTIME_DIR has no runtime.json: ${KETPLUS_BROWSER_RUNTIME_DIR}")
            endif()
            file(READ "${KETPLUS_BROWSER_RUNTIME_DIR}/runtime.json" ketplus_browser_runtime_json)
            string(JSON ketplus_browser_runtime_name ERROR_VARIABLE ketplus_json_error
                GET "${ketplus_browser_runtime_json}" name)
            string(JSON ketplus_browser_runtime_node ERROR_VARIABLE ketplus_json_error
                GET "${ketplus_browser_runtime_json}" nodeVersion)
            string(JSON ketplus_browser_runtime_staged_platform ERROR_VARIABLE ketplus_json_error
                GET "${ketplus_browser_runtime_json}" platform)
            if(NOT ketplus_browser_runtime_name STREQUAL "ketplus-browser"
               OR NOT ketplus_browser_runtime_node STREQUAL ketplus_node_pin_version
               OR NOT ketplus_browser_runtime_staged_platform STREQUAL ketplus_browser_runtime_platform)
                message(FATAL_ERROR
                    "KETPLUS_BROWSER_RUNTIME_DIR ${KETPLUS_BROWSER_RUNTIME_DIR} is "
                    "'${ketplus_browser_runtime_name}' node '${ketplus_browser_runtime_node}' "
                    "platform '${ketplus_browser_runtime_staged_platform}'; this build needs "
                    "ketplus-browser node ${ketplus_node_pin_version} ${ketplus_browser_runtime_platform}. "
                    "Re-stage it with apps/browser/scripts/package-runtime.mjs.")
            endif()
            option(KETPLUS_BROWSER_RUNTIME_ALLOW_STALE
                "Bundle a prebuilt browser runtime whose sources differ from this checkout" OFF)
            ketplus_browser_check_runtime("${ketplus_browser_app_dir}"
                "${KETPLUS_BROWSER_RUNTIME_DIR}" "${KETPLUS_BROWSER_RUNTIME_ALLOW_STALE}")
            set(ketplus_browser_runtime_stage "${KETPLUS_BROWSER_RUNTIME_DIR}")
            set(ketplus_browser_runtime_stamp "${KETPLUS_BROWSER_RUNTIME_DIR}/runtime.json")
        else()
            find_program(KETPLUS_NODE_EXECUTABLE node
                HINTS /opt/homebrew/bin /usr/local/bin
                DOC "Build-host Node.js used only to run package-runtime.mjs")
            if(NOT KETPLUS_NODE_EXECUTABLE)
                message(FATAL_ERROR
                    "KETPLUS_BUNDLE_BROWSER_RUNTIME needs a build-host node or KETPLUS_BROWSER_RUNTIME_DIR")
            endif()
            set(ketplus_browser_runtime_stage "${CMAKE_BINARY_DIR}/browser-runtime/ketplus-browser")
            set(ketplus_browser_runtime_stamp "${CMAKE_BINARY_DIR}/browser-runtime/staged.stamp")
            file(GLOB_RECURSE ketplus_browser_runtime_inputs CONFIGURE_DEPENDS
                "${ketplus_browser_app_dir}/src/*"
                "${ketplus_browser_app_dir}/.claude/skills/ketplus-browser/*"
            )
            set(ketplus_node_cache_argument)
            if(KETPLUS_NODE_CACHE)
                set(ketplus_node_cache_argument --node-cache "${KETPLUS_NODE_CACHE}")
            endif()
            add_custom_command(
                # runtime.json is an output too, so a deleted or partial stage dir is
                # re-staged even when the stamp survives.
                OUTPUT "${ketplus_browser_runtime_stamp}" "${ketplus_browser_runtime_stage}/runtime.json"
                COMMAND "${KETPLUS_NODE_EXECUTABLE}"
                    "${ketplus_browser_app_dir}/scripts/package-runtime.mjs"
                    --out "${ketplus_browser_runtime_stage}" ${ketplus_node_cache_argument}
                COMMAND "${CMAKE_COMMAND}" -E touch "${ketplus_browser_runtime_stamp}"
                DEPENDS
                    "${ketplus_browser_app_dir}/scripts/package-runtime.mjs"
                    "${ketplus_browser_app_dir}/runtime/node-runtime.json"
                    "${ketplus_browser_app_dir}/package.json"
                    "${ketplus_browser_app_dir}/package-lock.json"
                    ${ketplus_browser_runtime_inputs}
                COMMENT "Staging bundled browser runtime"
                VERBATIM
            )
        endif()

        # Sources or the runtime can change after configure: check again before every bundle
        # and install (#94). A custom target always runs.
        set(ketplus_browser_runtime_check
            "${CMAKE_COMMAND}" "-DKETPLUS_BROWSER_APP_DIR=${ketplus_browser_app_dir}"
            "-DKETPLUS_BROWSER_RUNTIME_DIR=${ketplus_browser_runtime_stage}"
            "-DKETPLUS_BROWSER_RUNTIME_ALLOW_STALE=${KETPLUS_BROWSER_RUNTIME_ALLOW_STALE}"
            -P "${PROJECT_SOURCE_DIR}/cmake/KetPlusBrowserRuntime.cmake")
        add_custom_target(ketplus_check_browser_runtime
            COMMAND ${ketplus_browser_runtime_check}
            DEPENDS "${ketplus_browser_runtime_stamp}"
            COMMENT "Checking the browser runtime against this checkout"
            VERBATIM
        )
        set(ketplus_browser_runtime_bundle_dir
            "$<TARGET_BUNDLE_DIR:ketplus>/Contents/Resources/ketplus-browser")
        add_custom_target(ketplus_bundle_browser_runtime ALL
            COMMAND "${CMAKE_COMMAND}" -E rm -rf "${ketplus_browser_runtime_bundle_dir}"
            # cp -R keeps the executable bit of bin/node and never follows links.
            COMMAND /bin/cp -R "${ketplus_browser_runtime_stage}" "${ketplus_browser_runtime_bundle_dir}"
            DEPENDS ketplus ketplus_bundle_daemon ketplus_check_browser_runtime
                "${ketplus_browser_runtime_stamp}"
            COMMENT "Copying browser runtime into KetPlus.app"
            VERBATIM
        )
    endif()
endif()

install(TARGETS ketplus
    BUNDLE DESTINATION . COMPONENT Runtime
    RUNTIME DESTINATION bin
    COMPONENT Runtime
)

if(APPLE AND KETPLUS_BUNDLE_BROWSER_RUNTIME)
    # After install(TARGETS ketplus): the bundle it copies embeds the runtime of its last build,
    # which can predate the staged one. The staged runtime is checked, replaces the installed one
    # (no files of the older one remain), and what was installed is checked again (#94).
    set(ketplus_installed_browser_runtime
        "\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/KetPlus.app/Contents/Resources/ketplus-browser")
    foreach(ketplus_browser_runtime_phase IN ITEMS staged installed)
        if(ketplus_browser_runtime_phase STREQUAL "staged")
            set(ketplus_checked_runtime "${ketplus_browser_runtime_stage}")
        else()
            set(ketplus_checked_runtime "${ketplus_installed_browser_runtime}")
        endif()
        set(ketplus_browser_runtime_check_${ketplus_browser_runtime_phase} "
            execute_process(
                COMMAND \"${CMAKE_COMMAND}\"
                    \"-DKETPLUS_BROWSER_APP_DIR=${ketplus_browser_app_dir}\"
                    \"-DKETPLUS_BROWSER_RUNTIME_DIR=${ketplus_checked_runtime}\"
                    \"-DKETPLUS_BROWSER_RUNTIME_ALLOW_STALE=${KETPLUS_BROWSER_RUNTIME_ALLOW_STALE}\"
                    -P \"${PROJECT_SOURCE_DIR}/cmake/KetPlusBrowserRuntime.cmake\"
                RESULT_VARIABLE ketplus_check_result)
            if(NOT ketplus_check_result EQUAL 0)
                message(FATAL_ERROR
                    \"The ${ketplus_browser_runtime_phase} browser runtime does not match this checkout\")
            endif()")
    endforeach()
    install(CODE "${ketplus_browser_runtime_check_staged}
        file(REMOVE_RECURSE \"${ketplus_installed_browser_runtime}\")"
        COMPONENT Runtime
    )
    install(DIRECTORY "${ketplus_browser_runtime_stage}/"
        DESTINATION "KetPlus.app/Contents/Resources/ketplus-browser"
        USE_SOURCE_PERMISSIONS
        COMPONENT Runtime
    )
    install(CODE "${ketplus_browser_runtime_check_installed}" COMPONENT Runtime)
endif()

if(APPLE)
    # After install(TARGETS ketplus) for the same reason: the bundle embeds the Claude sidecar and
    # the default agent skills (ket-atlas, enterprise-grade, ket-design-system) of its last
    # build. Installed before it, a tree changed since was overwritten file by file with the
    # older one, and files the new one no longer has remained. Each now replaces the installed
    # copy (#124). Desktop and the daemon find the skills at ../Resources/agent-skills.
    install(CODE "file(REMOVE_RECURSE
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/KetPlus.app/Contents/Resources/claude-sidecar\"
        \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/KetPlus.app/Contents/Resources/agent-skills\")"
        COMPONENT Runtime
    )
    install(DIRECTORY "${KETPLUS_CLAUDE_SIDECAR_STAGE}/"
        DESTINATION "KetPlus.app/Contents/Resources/claude-sidecar"
        USE_SOURCE_PERMISSIONS
        COMPONENT Runtime
    )
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/assets/agent-skills"
        DESTINATION "KetPlus.app/Contents/Resources"
        COMPONENT Runtime
    )
endif()

if(APPLE)
    install(TARGETS ketplus_daemon
        RUNTIME DESTINATION "KetPlus.app/Contents/Helpers"
        COMPONENT Runtime
    )
    if(KETPLUS_UPDATES_ENABLED)
        # ditto keeps the framework's symlinks and the signatures of its nested Autoupdate and
        # Updater.app. It must be in place before the Qt deploy script below runs.
        install(CODE "
            set(sparkle_destination \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/KetPlus.app/Contents/Frameworks/Sparkle.framework\")
            file(REMOVE_RECURSE \"\${sparkle_destination}\")
            execute_process(
                COMMAND /usr/bin/ditto \"${KETPLUS_SPARKLE_FRAMEWORK}\" \"\${sparkle_destination}\"
                RESULT_VARIABLE sparkle_copy_result)
            if(NOT sparkle_copy_result EQUAL 0)
                message(FATAL_ERROR \"Unable to embed Sparkle.framework\")
            endif()"
            COMPONENT Runtime
        )
    endif()
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        DESTINATION "KetPlus.app/Contents/Resources/licenses"
        COMPONENT Runtime
    )
    install(DIRECTORY "${PROJECT_SOURCE_DIR}/third_party/licenses/"
        DESTINATION "KetPlus.app/Contents/Resources/licenses"
        COMPONENT Runtime
    )

    # macdeployqt otherwise copies every plugin associated with a linked Qt
    # framework. Homebrew's Qt build includes optional PDF and virtual-keyboard
    # plugins whose frameworks are not installed, so deploy only the runtime
    # plugins KetPlus actually uses and pass them back to macdeployqt for rpath
    # rewriting and signing.
    function(ketplus_install_qt_plugin target plugin_type)
        if(NOT TARGET "${target}")
            message(FATAL_ERROR "Required Qt plugin target is unavailable: ${target}")
        endif()
        get_target_property(plugin_source "${target}" IMPORTED_LOCATION_RELEASE)
        if(NOT plugin_source)
            get_target_property(plugin_source "${target}" IMPORTED_LOCATION)
        endif()
        if(NOT plugin_source)
            message(FATAL_ERROR "Cannot locate required Qt plugin: ${target}")
        endif()
        file(REAL_PATH "${plugin_source}" plugin_real_path)
        get_filename_component(plugin_name "${plugin_real_path}" NAME)
        install(PROGRAMS "${plugin_real_path}"
            DESTINATION "KetPlus.app/Contents/PlugIns/${plugin_type}"
            COMPONENT Runtime
        )
        set(ketplus_installed_qt_plugins
            ${ketplus_installed_qt_plugins}
            "KetPlus.app/Contents/PlugIns/${plugin_type}/${plugin_name}"
            PARENT_SCOPE
        )
    endfunction()

    set(ketplus_installed_qt_plugins)
    ketplus_install_qt_plugin(Qt6::QCocoaIntegrationPlugin platforms)
    ketplus_install_qt_plugin(Qt6::QSQLiteDriverPlugin sqldrivers)
    ketplus_install_qt_plugin(Qt6::QSvgIconPlugin iconengines)
    ketplus_install_qt_plugin(Qt6::QGifPlugin imageformats)
    ketplus_install_qt_plugin(Qt6::QJpegPlugin imageformats)
    ketplus_install_qt_plugin(Qt6::QSvgPlugin imageformats)
    ketplus_install_qt_plugin(Qt6::QAppleNetworkInformationPlugin networkinformation)
    ketplus_install_qt_plugin(Qt6::QSecureTransportBackendPlugin tls)
    ketplus_install_qt_plugin(Qt6::QMacStylePlugin styles)
    ketplus_install_qt_plugin(Qt6::QDarwinWebViewPlugin webview)
    list(JOIN ketplus_installed_qt_plugins " " ketplus_installed_qt_plugin_arguments)

    set_property(TARGET ketplus PROPERTY QT_QML_IMPORT_SCANNER_EXTRA_ROOT_PATHS
        "${PROJECT_SOURCE_DIR}/modules/ai/src/agent/qml"
    )
    # Use an explicit deploy script so Homebrew's symlinked QML plugins can be
    # materialized before macdeployqt inspects and rewrites their dependencies.
    # Copying those symlinks verbatim would leave a package tied to /opt/homebrew.
    qt6_generate_deploy_script(
        TARGET ketplus
        NAME ketplus_qml_app
        OUTPUT_SCRIPT ketplus_qt_deploy_script
        CONTENT "
qt6_deploy_qml_imports(TARGET ketplus PLUGINS_FOUND plugins_found)
set(KETPLUS_QML_IMPORTS_FILE
    \"${CMAKE_CURRENT_BINARY_DIR}/.qt/qml_imports/ketplus_build.cmake\")
set(KETPLUS_BUNDLE_PLUGIN_DIR
    \"\${QT_DEPLOY_PREFIX}/KetPlus.app/Contents/PlugIns\")
include(\"${PROJECT_SOURCE_DIR}/packaging/macos/MaterializeQmlPlugins.cmake\")
qt6_deploy_runtime_dependencies(
    EXECUTABLE \"KetPlus.app\"
    ADDITIONAL_MODULES \${plugins_found} ${ketplus_installed_qt_plugin_arguments}
    NO_PLUGINS
    NO_TRANSLATIONS
    DEPLOY_TOOL_OPTIONS
        \"-executable=KetPlus.app/Contents/Helpers/ketplus_daemon\"
)
set(KETPLUS_APP_BUNDLE \"\${QT_DEPLOY_PREFIX}/KetPlus.app\")
include(\"${PROJECT_SOURCE_DIR}/packaging/macos/SignDeployedFrameworks.cmake\")"
    )
    install(SCRIPT "${ketplus_qt_deploy_script}" COMPONENT Runtime)

    configure_file(
        "${PROJECT_SOURCE_DIR}/packaging/macos/VerifyInstalledBundle.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/VerifyInstalledBundle.cmake"
        @ONLY
    )
    install(SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/VerifyInstalledBundle.cmake"
        COMPONENT Runtime
    )
endif()

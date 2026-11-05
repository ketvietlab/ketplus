if(NOT DEFINED KETPLUS_QML_IMPORTS_FILE OR
   NOT EXISTS "${KETPLUS_QML_IMPORTS_FILE}")
    message(FATAL_ERROR "KetPlus QML import scan is unavailable")
endif()
if(NOT DEFINED KETPLUS_BUNDLE_PLUGIN_DIR)
    message(FATAL_ERROR "KetPlus bundle plugin directory is unavailable")
endif()
get_filename_component(ketplus_bundle_contents_dir
    "${KETPLUS_BUNDLE_PLUGIN_DIR}" DIRECTORY)
set(KETPLUS_BUNDLE_QML_DIR "${ketplus_bundle_contents_dir}/Resources/qml")

include("${KETPLUS_QML_IMPORTS_FILE}")

if(qml_import_scanner_imports_count GREATER 0)
    math(EXPR last_import "${qml_import_scanner_imports_count} - 1")
    foreach(index RANGE 0 ${last_import})
        cmake_parse_arguments(entry ""
            "CLASSNAME;NAME;PATH;PLUGIN;RELATIVEPATH;TYPE;VERSION;LINKTARGET"
            ""
            ${qml_import_scanner_import_${index}}
        )

        if(entry_PATH AND entry_RELATIVEPATH)
            set(qmldir_source "${entry_PATH}/qmldir")
            set(qmldir_destination
                "${KETPLUS_BUNDLE_QML_DIR}/${entry_RELATIVEPATH}/qmldir")
            if(IS_SYMLINK "${qmldir_destination}")
                file(REAL_PATH "${qmldir_source}" qmldir_real_path)
                file(REMOVE "${qmldir_destination}")
                file(COPY_FILE "${qmldir_real_path}" "${qmldir_destination}")
            endif()
        endif()

        if(NOT entry_PATH OR NOT entry_PLUGIN)
            continue()
        endif()

        file(GLOB plugin_sources LIST_DIRECTORIES false
            "${entry_PATH}/*${entry_PLUGIN}*.dylib"
        )
        foreach(plugin_source IN LISTS plugin_sources)
            get_filename_component(plugin_name "${plugin_source}" NAME)
            set(plugin_destination "${KETPLUS_BUNDLE_PLUGIN_DIR}/${plugin_name}")
            if(IS_SYMLINK "${plugin_destination}")
                file(REAL_PATH "${plugin_source}" plugin_real_path)
                file(REMOVE "${plugin_destination}")
                file(COPY_FILE "${plugin_real_path}" "${plugin_destination}")
            endif()
        endforeach()
    endforeach()
endif()

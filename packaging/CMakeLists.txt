if(NOT KETPLUS_BUILD_DESKTOP)
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGE_NAME "KetPlus-Daemon")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    set(CPACK_PACKAGE_FILE_NAME "KetPlus-Daemon-${PROJECT_VERSION}-${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")
elseif(APPLE)
    set(ketplus_dmg_background_svg
        "${CMAKE_CURRENT_BINARY_DIR}/dmg-background.svg"
    )
    set(ketplus_dmg_background_2x
        "${CMAKE_CURRENT_BINARY_DIR}/dmg-background@2x.png"
    )
    set(ketplus_dmg_background_1x
        "${CMAKE_CURRENT_BINARY_DIR}/dmg-background.png"
    )
    set(ketplus_dmg_background
        "${CMAKE_CURRENT_BINARY_DIR}/dmg-background.tiff"
    )
    # sips renders SVG without app fonts, so the lockup arrives as a pre-rendered @2x PNG.
    set(ketplus_dmg_lockup "${CMAKE_CURRENT_SOURCE_DIR}/macos/dmg-lockup.png")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${ketplus_dmg_lockup}")
    execute_process(
        COMMAND /usr/bin/base64 -i "${ketplus_dmg_lockup}"
        OUTPUT_VARIABLE KETPLUS_DMG_LOCKUP_BASE64
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE ketplus_dmg_lockup_result
    )
    execute_process(
        COMMAND /usr/bin/sips -g pixelWidth -g pixelHeight "${ketplus_dmg_lockup}"
        OUTPUT_VARIABLE ketplus_dmg_lockup_info
        RESULT_VARIABLE ketplus_dmg_lockup_info_result
    )
    if(NOT ketplus_dmg_lockup_result EQUAL 0 OR NOT ketplus_dmg_lockup_info_result EQUAL 0 OR
       NOT ketplus_dmg_lockup_info MATCHES "pixelWidth: ([0-9]+).*pixelHeight: ([0-9]+)")
        message(FATAL_ERROR "Unable to read the DMG lockup ${ketplus_dmg_lockup}")
    endif()
    math(EXPR KETPLUS_DMG_LOCKUP_WIDTH "${CMAKE_MATCH_1} / 2")
    math(EXPR KETPLUS_DMG_LOCKUP_HEIGHT "${CMAKE_MATCH_2} / 2")
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/macos/dmg-background.svg.in"
        "${ketplus_dmg_background_svg}"
        @ONLY
    )
    execute_process(
        COMMAND /usr/bin/sips -s format png "${ketplus_dmg_background_svg}"
            --out "${ketplus_dmg_background_2x}"
        RESULT_VARIABLE ketplus_dmg_render_result
        OUTPUT_QUIET
        ERROR_VARIABLE ketplus_dmg_render_error
    )
    if(NOT ketplus_dmg_render_result EQUAL 0)
        message(FATAL_ERROR "Unable to render the DMG background: ${ketplus_dmg_render_error}")
    endif()
    execute_process(
        COMMAND /usr/bin/sips -z 420 660 "${ketplus_dmg_background_2x}"
            --out "${ketplus_dmg_background_1x}"
        RESULT_VARIABLE ketplus_dmg_resize_result
        OUTPUT_QUIET
        ERROR_VARIABLE ketplus_dmg_resize_error
    )
    if(NOT ketplus_dmg_resize_result EQUAL 0)
        message(FATAL_ERROR "Unable to resize the DMG background: ${ketplus_dmg_resize_error}")
    endif()
    execute_process(
        COMMAND /usr/bin/tiffutil -cathidpicheck
            "${ketplus_dmg_background_1x}"
            "${ketplus_dmg_background_2x}"
            -out "${ketplus_dmg_background}"
        RESULT_VARIABLE ketplus_dmg_tiff_result
        OUTPUT_QUIET
        ERROR_VARIABLE ketplus_dmg_tiff_error
    )
    if(NOT ketplus_dmg_tiff_result EQUAL 0)
        message(FATAL_ERROR "Unable to create the retina DMG background: ${ketplus_dmg_tiff_error}")
    endif()

    set(CPACK_GENERATOR "DragNDrop")
    set(CPACK_PACKAGE_NAME "KetPlus")
    set(CPACK_PACKAGE_VENDOR "Ketsuite")
    set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
    include("${PROJECT_SOURCE_DIR}/cmake/KetPlusMacPackageArchitecture.cmake")
    ketplus_mac_package_architecture(ketplus_dmg_architecture)
    set(CPACK_PACKAGE_FILE_NAME "KetPlus-${PROJECT_VERSION}-${ketplus_dmg_architecture}")
    set(CPACK_DMG_VOLUME_NAME "KetPlus")
    set(CPACK_DMG_BACKGROUND_IMAGE "${ketplus_dmg_background}")
    set(CPACK_DMG_DS_STORE
        "${CMAKE_CURRENT_SOURCE_DIR}/macos/DmgLayout.DS_Store"
    )
    set(CPACK_MONOLITHIC_INSTALL ON)
elseif(WIN32)
    set(CPACK_GENERATOR "ZIP")
    set(CPACK_PACKAGE_FILE_NAME "KetPlus-${PROJECT_VERSION}-windows-${CMAKE_SYSTEM_PROCESSOR}")
elseif(UNIX)
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGE_FILE_NAME "KetPlus-${PROJECT_VERSION}-linux-${CMAKE_SYSTEM_PROCESSOR}")
endif()

include(CPack)

cmake_minimum_required(VERSION 3.24)

# Also support a small standalone build: no Desktop/daemon or provider runtime is built.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(KetPlusUpdaterPolicyTests LANGUAGES CXX OBJCXX)
    enable_testing()
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_AUTOMOC ON)
    find_package(Qt6 REQUIRED COMPONENTS Core)
    if(NOT EXISTS "${KETPLUS_SPARKLE_DIR}/Sparkle.framework/Headers/SPUUpdater.h")
        message(FATAL_ERROR "KETPLUS_SPARKLE_DIR must name the pinned Sparkle SDK")
    endif()
    set(updater_source "${CMAKE_CURRENT_SOURCE_DIR}/../../apps/desktop/src")
    add_library(ketplus_app_updater STATIC
        "${updater_source}/update/AppUpdater.cpp"
        "${updater_source}/update/AppUpdater.h"
        "${updater_source}/update/SparkleAppUpdater.mm")
    target_include_directories(ketplus_app_updater PUBLIC "${updater_source}")
    target_compile_definitions(ketplus_app_updater PRIVATE KETPLUS_HAS_SPARKLE=1)
    target_compile_options(ketplus_app_updater PUBLIC "-F${KETPLUS_SPARKLE_DIR}")
    target_link_options(ketplus_app_updater PUBLIC "-F${KETPLUS_SPARKLE_DIR}")
    target_link_libraries(ketplus_app_updater PUBLIC Qt6::Core "-framework Sparkle" "-framework Foundation")
    set_source_files_properties("${updater_source}/update/SparkleAppUpdater.mm"
        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
endif()

qt_add_executable(ketplus_updater_feed_policy_tests MACOSX_BUNDLE UpdaterFeedPolicyTest.mm)
set_source_files_properties(UpdaterFeedPolicyTest.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_link_libraries(ketplus_updater_feed_policy_tests PRIVATE ketplus_app_updater)
set_target_properties(ketplus_updater_feed_policy_tests PROPERTIES
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in"
    BUILD_RPATH "${KETPLUS_SPARKLE_DIR}")
add_test(NAME ketplus_updater_feed_policy_tests COMMAND "$<TARGET_FILE:ketplus_updater_feed_policy_tests>")
set_tests_properties(ketplus_updater_feed_policy_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

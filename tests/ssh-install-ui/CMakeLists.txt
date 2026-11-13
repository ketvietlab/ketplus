# Focused offscreen Connections proof without the full editor's WebView/compat modules.
# Uses the same kit revision as core/cmake/Dependencies.cmake, and the actual production files.
find_package(Qt6 6.11 REQUIRED COMPONENTS Widgets Svg SvgWidgets Test)
include(FetchContent)
set(KETQT_BUILD_QUICK OFF CACHE BOOL "" FORCE)
set(KETQT_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(KETQT_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(KETQT_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ketqt
    GIT_REPOSITORY https://github.com/ketvietlab/ket-qt.git
    GIT_TAG cf3ac4e97b1db4566568887e02bdf681012373d5)
FetchContent_MakeAvailable(ketqt)
qt_add_executable(ketplus_ssh_install_ui_tests
    SshInstallUiTest.cpp
    ../../apps/desktop/src/remote/LinuxConnectDialog.cpp
    ../../apps/desktop/src/remote/LinuxDaemonInstallCard.cpp
    ../../apps/desktop/src/remote/LinuxMachineRow.cpp
    ../../apps/desktop/src/remote/LinuxMachineModel.cpp
    ../../apps/desktop/src/remote/LinuxSshMachines.cpp
    ../../apps/desktop/src/remote/LinuxSshInstall.cpp)
target_include_directories(ketplus_ssh_install_ui_tests PRIVATE
    ../../apps/desktop/src ../daemon)
target_link_libraries(ketplus_ssh_install_ui_tests PRIVATE Qt6::Test Qt6::Widgets
    KetQt::Widgets ketplus_remote_ssh_backend ketplus_remote_control)
target_compile_definitions(ketplus_ssh_install_ui_tests PRIVATE
    KETPLUS_TEST_CTL="$<TARGET_FILE:ketplusctl>")
add_dependencies(ketplus_ssh_install_ui_tests ketplusctl)
ketplus_enable_warnings(ketplus_ssh_install_ui_tests)
add_test(NAME ketplus_ssh_install_ui_tests COMMAND ketplus_ssh_install_ui_tests)
set_tests_properties(ketplus_ssh_install_ui_tests PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

# Compiler options every KetPlus target needs under MSVC. Included by core/CMakeLists.txt and, for
# Pro, by the top-level CMakeLists.txt before its subdirectories, so modules/ and apps/ get them
# too. include_guard(DIRECTORY) makes the second include a no-op inside the same tree.
include_guard(DIRECTORY)

if(MSVC)
    # Sources are UTF-8 (string literals with non-ASCII text); MSVC otherwise reads the ANSI code
    # page. /Zc:__cplusplus makes __cplusplus honest for Qt (C++ only, so the C libvterm and miniz
    # do not get it), /bigobj covers the large moc and editor units, and NOMINMAX keeps
    # <windows.h> from shadowing std::min and std::max.
    add_compile_options(/utf-8 "$<$<COMPILE_LANGUAGE:CXX>:/Zc:__cplusplus>" /bigobj)
    add_compile_definitions(NOMINMAX _CRT_SECURE_NO_WARNINGS)
    # Incremental linking writes a roughly 180 MB .ilk next to every executable; with the
    # hundreds of test executables that filled a build machine's disk. Full links are fast enough.
    add_link_options(/INCREMENTAL:NO)
endif()

# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright 2026 the ScarletNX contributors
# The macOS app and the iOS one (docs/APPLE.md), made by `cmake --install` and packed by CPack. Run on a Mac with Xcode.
#
#   cpack --config build/macos/CPackConfig.cmake   ScarletNX-macos-arm64.zip of ScarletNX.app, signed ad hoc
#   cpack --config build/ios/CPackConfig.cmake     ScarletNX-ios-arm64.ipa, unsigned, for a sideloading tool to sign
#
# `-G DragNDrop` makes the macOS app a disk image instead, and `-D CPACK_PACKAGE_FILE_NAME=...` names the package.
# On macOS the install prefix is the app's Contents (CMakeLists.txt puts the programs in MacOS/ and share/ beside it,
# where they look: ../share/scarletnx), so `cmake --install build/macos --prefix ScarletNX.app/Contents` makes the app
# too. On iOS the app is the bundle the build makes, with share/ in it.
#
# SNX_MOLTENVK is a MoltenVK release's MoltenVK-macos.tar or MoltenVK-ios.tar, extracted: Ryujinx's (V380-Ori/
# Ryujinx.MoltenVK, which .github/workflows/build.yml pins), whose patches emulators' pipelines need. Its dynamic
# library goes in the app's Frameworks, where the runtime has SDL load it (backend/sdl3.c). Without it the macOS app
# uses the system's Vulkan loader (Homebrew's molten-vk), and the iOS app has none.
set(SNX_MOLTENVK "" CACHE PATH "An extracted MoltenVK release whose library the Apple app carries (empty: none)")

# the file or folder of the MoltenVK release under dynamic/ whose path ends as regex says, into var; and its license,
# installed beside the app's other ones
function(snx_moltenvk_part regex var)
    file(GLOB_RECURSE found LIST_DIRECTORIES true "${SNX_MOLTENVK}/*")
    list(FILTER found INCLUDE REGEX "/dynamic/(.*/)?${regex}$")
    if(NOT found)
        message(FATAL_ERROR "SNX_MOLTENVK=${SNX_MOLTENVK} has no ${regex} under dynamic/")
    endif()
    list(GET found 0 part)
    set(${var} "${part}" PARENT_SCOPE)
    file(GLOB_RECURSE license "${SNX_MOLTENVK}/LICENSE")
    if(license)
        list(GET license 0 license)
        install(FILES "${license}" DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/scarletnx/MoltenVK)
    endif()
endfunction()

set(CPACK_PACKAGE_NAME ScarletNX)
set(CPACK_PACKAGE_VENDOR "the ScarletNX contributors")
set(CPACK_INCLUDE_TOPLEVEL_DIRECTORY OFF)
if(IOS)
    install(FILES dist/icons/hicolor/256x256/apps/wimsy.png DESTINATION scarletnx.app RENAME AppIcon.png)
    if(SNX_MOLTENVK)
        snx_moltenvk_part("ios-arm64/MoltenVK\\.framework" framework)
        install(DIRECTORY "${framework}" DESTINATION scarletnx.app/Frameworks USE_SOURCE_PERMISSIONS)
    endif()
    # an IPA is a zip of Payload/<name>.app
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGING_INSTALL_PREFIX /Payload)
    set(CPACK_ARCHIVE_FILE_EXTENSION ipa)
    set(CPACK_PACKAGE_FILE_NAME ScarletNX-ios-arm64)
else()
    # the icon, drawn from the PNG with macOS's own tools
    set(png "${PROJECT_SOURCE_DIR}/dist/icons/hicolor/256x256/apps/wimsy.png")
    set(iconset "${PROJECT_BINARY_DIR}/ScarletNX.iconset")
    set(icns "${PROJECT_BINARY_DIR}/ScarletNX.icns")
    set(sizes)
    foreach(size 16 32 128 256)
        list(APPEND sizes COMMAND sips -z ${size} ${size} "${png}" --out "${iconset}/icon_${size}x${size}.png")
    endforeach()
    add_custom_command(OUTPUT "${icns}" COMMAND "${CMAKE_COMMAND}" -E make_directory "${iconset}" ${sizes}
                       COMMAND iconutil -c icns "${iconset}" -o "${icns}" DEPENDS "${png}" VERBATIM)
    add_custom_target(snx_macos_icon ALL DEPENDS "${icns}")
    install(FILES dist/apple/Info-macOS.plist DESTINATION . RENAME Info.plist)
    install(FILES "${icns}" DESTINATION Resources)
    if(SNX_MOLTENVK)
        snx_moltenvk_part("dylib/macOS/libMoltenVK\\.dylib" dylib)
        install(FILES "${dylib}" DESTINATION Frameworks)
    endif()
    # Apple silicon runs no unsigned code: the app signed ad hoc, for the Mac it is copied to, after everything else is
    # in it (the rules run in the order they are declared, CMP0082, and this is the last). Only when the prefix is an
    # app's Contents
    install(CODE [[
        set(contents "$ENV{DESTDIR}${CMAKE_INSTALL_PREFIX}")
        if(contents MATCHES "\\.app/Contents/?$")
            get_filename_component(app "${contents}" DIRECTORY)
            execute_process(COMMAND codesign --force --deep --sign - "${app}" COMMAND_ERROR_IS_FATAL ANY)
        endif()
    ]])
    # the zip the releases carry (scripts/publish-builds.py); its symbolic links (scarletnx-switch) stay links
    set(CPACK_GENERATOR ZIP)
    set(CPACK_PACKAGING_INSTALL_PREFIX /ScarletNX.app/Contents)
    set(CPACK_DMG_VOLUME_NAME ScarletNX)
    set(CPACK_PACKAGE_FILE_NAME ScarletNX-macos-arm64)
endif()
include(CPack)

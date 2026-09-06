# What an installed AX310 consists of, and how to build a package of it.
#
# Three of the four files are configuration for other daemons, and none of them
# is optional: without the udev rule the deck cannot be opened at all, and without
# the two PipeWire fragments it presents one six-channel device instead of six
# tracks. They live in packaging/ rather than being generated at install time so
# that a package and a development machine run the same bytes -- scripts/setup-
# audio.sh installs these very files into the user's own configuration
# directories, which is what makes the development path a rehearsal for the
# package rather than a separate thing that drifts.

include(GNUInstallDirs)

install(TARGETS ax310_app RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

# udev rules go to lib/udev/rules.d on every distribution, including the ones
# where CMAKE_INSTALL_LIBDIR is lib64. This is not a library directory.
install(FILES "${PROJECT_SOURCE_DIR}/packaging/udev/70-ax310.rules"
        DESTINATION "lib/udev/rules.d")

install(FILES "${PROJECT_SOURCE_DIR}/packaging/pipewire/ax310-split.conf"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/pipewire/pipewire.conf.d")

install(FILES "${PROJECT_SOURCE_DIR}/packaging/wireplumber/51-ax310.conf"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/wireplumber/wireplumber.conf.d")

install(FILES "${PROJECT_SOURCE_DIR}/packaging/ax310.desktop"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")

install(FILES "${PROJECT_SOURCE_DIR}/LICENSE"
              "${PROJECT_SOURCE_DIR}/NOTICE"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}")

# The two typefaces are compiled into the binary as Qt resources, so they are not
# installed as files -- but their licences travel with any binary that carries
# them, so those are.
install(FILES "${PROJECT_SOURCE_DIR}/src/gui/fonts/OFL-BarlowCondensed.txt"
              "${PROJECT_SOURCE_DIR}/src/gui/fonts/OFL-JetBrainsMono.txt"
        DESTINATION "${CMAKE_INSTALL_DOCDIR}/fonts")

set(CPACK_PACKAGE_NAME "ax310")
set(CPACK_PACKAGE_VENDOR "Christian Parpart")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Driver and interface for the AVerMedia Live Streamer AX310 control deck")
set(CPACK_PACKAGE_CONTACT "Christian Parpart <christian@parpart.family>")
set(CPACK_PACKAGE_HOMEPAGE_URL "https://github.com/christianparpart/ax310")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGING_INSTALL_PREFIX "/usr")
set(CPACK_STRIP_FILES ON)

set(CPACK_RESOURCE_FILE_LICENSE "${PROJECT_SOURCE_DIR}/LICENSE")

# The project's own licence, not the dependencies'. What this links against and
# on what terms -- hidapi's BSD-style option, Qt under LGPL-3.0, and the two
# OFL-1.1 typefaces compiled into the binary -- is in NOTICE, which installs
# beside it.
set(CPACK_RPM_PACKAGE_LICENSE "Apache-2.0")

# These directories belong to systemd, pipewire and wireplumber. Claiming them
# would make this package conflict with the ones that own them.
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION
    "/usr/lib/udev"
    "/usr/lib/udev/rules.d"
    "/usr/share/pipewire"
    "/usr/share/pipewire/pipewire.conf.d"
    "/usr/share/wireplumber"
    "/usr/share/wireplumber/wireplumber.conf.d"
    "/usr/share/applications")

set(CPACK_RPM_PACKAGE_REQUIRES "pipewire, wireplumber")
set(CPACK_DEBIAN_PACKAGE_DEPENDS "pipewire, wireplumber")
set(CPACK_DEBIAN_PACKAGE_SECTION "sound")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)

# Not set here: the generator. `cpack -G RPM` and `cpack -G DEB` pick one, and a
# machine that can build only one of them should not be told to try both.
#
# The .deb has to be built where dpkg is. CPack will produce one without it, but
# dpkg-shlibdeps cannot run, so the package declares only the dependencies named
# above -- and the architecture falls back to i386, which is wrong everywhere
# this runs. It says so while it does it; do not ship that package.
include(CPack)

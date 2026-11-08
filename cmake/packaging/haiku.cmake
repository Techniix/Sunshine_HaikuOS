# haiku specific packaging
#
# Default layout follows Haiku's non-packaged hierarchy, so a plain `cmake --install` works without
# building an .hpkg:
#   <prefix>/bin/sunshine
#   <prefix>/<SUNSHINE_ASSETS_DIR>/...                 (web UI, apps.json, ...)
#   <prefix>/add-ons/input_server/devices/virtualhid_input   (libvirtualhid keyboard/mouse add-on)
# with <prefix> = /boot/home/config/non-packaged (see packaging/haiku/build.sh).

install(DIRECTORY "${SUNSHINE_SOURCE_ASSETS_DIR}/haiku/assets/"
        DESTINATION "${SUNSHINE_ASSETS_DIR}")

# copy assets to the build directory, for running without install
file(COPY "${SUNSHINE_SOURCE_ASSETS_DIR}/haiku/assets/"
        DESTINATION "${CMAKE_BINARY_DIR}/assets")

# libvirtualhid's Haiku input_server add-on, which delivers streamed keyboard and mouse input.
if(TARGET libvirtualhid_haiku_input_addon)
    install(TARGETS libvirtualhid_haiku_input_addon
            LIBRARY DESTINATION "add-ons/input_server/devices")
endif()

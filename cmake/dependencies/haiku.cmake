# haiku specific dependencies

# Keyboard and mouse injection is provided by libvirtualhid's Haiku backend and its
# input_server add-on (third-party/libvirtualhid/src/platform/haiku). Make sure the add-on is
# built; Sunshine installs it next to the executable (see packaging/haiku.cmake).
set(LIBVIRTUALHID_BUILD_HAIKU_INPUT_ADDON ON CACHE BOOL "Build the Haiku input_server add-on" FORCE)

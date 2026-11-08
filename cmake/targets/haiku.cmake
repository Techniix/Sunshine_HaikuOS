# haiku specific target definitions

# Build the libvirtualhid input_server add-on together with Sunshine.
if(TARGET libvirtualhid_haiku_input_addon)
    add_dependencies(sunshine libvirtualhid_haiku_input_addon)
endif()

# Haiku message codes are four-character constants, which GCC reports with -Wmultichar.
list(APPEND SUNSHINE_COMPILE_OPTIONS -Wno-multichar)

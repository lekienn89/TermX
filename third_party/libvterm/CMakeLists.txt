# libvterm 0.3.3 (MIT) - vendored unmodified from upstream.
# Upstream: https://www.leonerd.org.uk/code/libvterm/
add_library(vterm STATIC
    src/encoding.c
    src/keyboard.c
    src/mouse.c
    src/parser.c
    src/pen.c
    src/screen.c
    src/state.c
    src/unicode.c
    src/vterm.c
)
target_include_directories(vterm PUBLIC include PRIVATE src)
set_target_properties(vterm PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON)
if(MSVC)
    target_compile_definitions(vterm PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(vterm PRIVATE /W0 /utf-8)
else()
    target_compile_options(vterm PRIVATE -w)
endif()

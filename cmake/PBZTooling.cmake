find_program(PBZ_CLANG_FORMAT clang-format REQUIRED)
set(PBZ_CPP_FILES
    mods/PBZEditor/inc/items/item_editor.hpp
    mods/PBZEditor/src/dllmain.cpp
    mods/PBZEditor/src/items/item_runtime.hpp
    mods/PBZEditor/src/items/item_runtime.cpp
    mods/PBZEditor/src/items/item_ui.hpp
    mods/PBZEditor/src/items/item_ui.cpp
    mods/PBZEditor/tests/item_tests.cpp)
list(TRANSFORM PBZ_CPP_FILES PREPEND "${CMAKE_SOURCE_DIR}/")
add_custom_target(format-check
    COMMAND "${PBZ_CLANG_FORMAT}" --dry-run --Werror ${PBZ_CPP_FILES}
    VERBATIM)
add_custom_target(format
    COMMAND "${PBZ_CLANG_FORMAT}" -i ${PBZ_CPP_FILES}
    VERBATIM)

find_program(PBZ_CLANG_FORMAT clang-format REQUIRED)
add_custom_target(format-check
    COMMAND "${PBZ_CLANG_FORMAT}" --dry-run --Werror "${CMAKE_SOURCE_DIR}/mods/PBZEditor/src/dllmain.cpp"
    VERBATIM)
add_custom_target(format
    COMMAND "${PBZ_CLANG_FORMAT}" -i "${CMAKE_SOURCE_DIR}/mods/PBZEditor/src/dllmain.cpp"
    VERBATIM)

add_library(PBZEditor SHARED
    src/dllmain.cpp
    src/items/item_runtime.cpp
    src/items/item_ui.cpp)
target_link_libraries(PBZEditor PRIVATE UE4SS)
target_include_directories(PBZEditor PRIVATE inc src)
target_compile_features(PBZEditor PRIVATE cxx_std_23)
target_compile_definitions(PBZEditor PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(PBZEditor PRIVATE /utf-8)

add_executable(PBZItemTests tests/item_tests.cpp)
target_include_directories(PBZItemTests PRIVATE inc)
target_compile_features(PBZItemTests PRIVATE cxx_std_23)
target_compile_options(PBZItemTests PRIVATE /utf-8)
add_test(NAME PBZItemTests COMMAND PBZItemTests)

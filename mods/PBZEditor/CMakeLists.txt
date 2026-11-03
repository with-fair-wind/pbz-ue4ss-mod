add_library(PBZEditor SHARED src/dllmain.cpp)
target_link_libraries(PBZEditor PRIVATE UE4SS)
target_compile_features(PBZEditor PRIVATE cxx_std_23)

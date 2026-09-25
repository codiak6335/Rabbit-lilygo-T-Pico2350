add_library(rabbit_core
    src/pool_geometry.cpp
    src/workout_engine.cpp
)

target_include_directories(rabbit_core PUBLIC include)
target_compile_features(rabbit_core PUBLIC cxx_std_17)
target_compile_options(rabbit_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

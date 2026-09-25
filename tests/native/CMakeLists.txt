add_executable(rabbit_native_tests test_main.cpp)
target_link_libraries(rabbit_native_tests PRIVATE rabbit_core rabbit_protocol)
target_compile_features(rabbit_native_tests PRIVATE cxx_std_17)
target_compile_options(rabbit_native_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

enable_testing()
add_test(NAME rabbit_native_tests COMMAND rabbit_native_tests)

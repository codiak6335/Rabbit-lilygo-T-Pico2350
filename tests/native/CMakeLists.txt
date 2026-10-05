add_executable(rabbit_native_tests test_main.cpp)
target_link_libraries(rabbit_native_tests PRIVATE rabbit_core rabbit_protocol)
target_compile_features(rabbit_native_tests PRIVATE cxx_std_17)
target_compile_options(rabbit_native_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)

enable_testing()
add_test(NAME rabbit_native_tests COMMAND rabbit_native_tests)

add_executable(rabbit_audio_tests test_audio.cpp)
target_link_libraries(rabbit_audio_tests PRIVATE rabbit_protocol)
target_compile_features(rabbit_audio_tests PRIVATE cxx_std_17)
target_compile_options(rabbit_audio_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
add_test(NAME rabbit_audio_tests COMMAND rabbit_audio_tests)

add_executable(rabbit_speaker_signal_tests test_speaker_signal.cpp)
target_include_directories(rabbit_speaker_signal_tests PRIVATE ../../firmware/rp2350/include)
target_compile_features(rabbit_speaker_signal_tests PRIVATE cxx_std_17)
target_compile_options(rabbit_speaker_signal_tests PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
add_test(NAME rabbit_speaker_signal_tests COMMAND rabbit_speaker_signal_tests)

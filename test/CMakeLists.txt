include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        v1.15.2
)
FetchContent_MakeAvailable(googletest)


add_executable(unit_test unit_test.cpp)

target_link_libraries(unit_test 
    gtest 
    gtest_main
    ylog
)

add_test(NAME unit_test COMMAND unit_test)
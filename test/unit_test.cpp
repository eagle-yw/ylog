
#include "gtest/gtest.h"

import std;
import ylog;

TEST(LoggerTest, DefaultLogger){

    auto oss = std::ostringstream();
    auto* original_buffer = std::cout.rdbuf();
    std::cout.rdbuf(oss.rdbuf());

    //test
    ylog::set_level(ylog::Level::Debug);
    ylog::debug("default logger level is {}", "debug");
    
    
    auto log_output = oss.str();
    auto except_output = std::string("[debug] [unit_test.cpp:15] default logger level is debug\n") ;
    auto is_ok = log_output.ends_with(except_output);
    EXPECT_TRUE(is_ok);

    
    //test end
    std::cout.rdbuf(original_buffer);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

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
    auto except_output = std::string("default logger level is debug\n") ;
    auto is_ok = log_output.ends_with(except_output);
    EXPECT_TRUE(is_ok);

    
    //test end
    std::cout.rdbuf(original_buffer);
}

TEST(LoggerTest, AppendSink){

    auto logger = ylog::Logger();
    logger.set_level(ylog::Level::Debug);

    std::string except_output = "append sink test";
    struct TestSink {
        void write(std::string_view msg) { 
            auto is_ok = msg.ends_with("append sink test\n");
            EXPECT_TRUE(is_ok);
            std::cout << msg;
        }
    };

    
    logger.append_sink(TestSink());
    logger.debug("{}", except_output);
}

TEST(LoggerTest, TestSinkFile){
    std::string except_output = "test sink file";
    auto sink =ylog::SinkFile("test.log");

    auto logger = ylog::Logger();
    logger.set_level(ylog::Level::Debug);
    logger.append_sink(std::move(sink));

    logger.debug("{}", except_output);
    logger.info("{}", except_output);
    logger.error("{}", except_output);
}

TEST(SinkTest, DefaultSink){
    struct TestSink {
        void write(std::string_view msg) { msg_ = msg;}
        std::string_view msg_;
    };
    std::vector<ylog::SinkIface> vec;
    vec.push_back(TestSink());

    std::string_view except_output = "hello world";
    vec[0].write(except_output);

    auto sink = vec[0].as<TestSink>();
    EXPECT_EQ(sink->msg_, except_output);
}

int main(int argc, char **argv) {
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
import std;
import ylog;

int main(int argc, char* argv[]){
    std::println("hello world!");
 
    auto logger = ylog::Logger();
    logger.set_level(ylog::Level::Debug);
    logger.debug("level is {}, {}", "debug", "009");
    logger.info("level is {}, {}", "info", "010");
    logger.warning("level is {}, {}", "warning", "010");
    logger.error("level is {}, {}", "error", "010");

    ylog::set_level(ylog::Level::Debug);
    ylog::debug("default logger level is {}, {}", "debug", "009");
    // ylog::error("default logger level is {}, {}", "error", "009");
    return 0;
}
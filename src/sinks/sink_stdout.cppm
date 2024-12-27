export module ylog:sink.stdout;

import std;
import :sink.file;

namespace ylog {

class SinksStdout {
public:
    SinksStdout(){}
    void write(std::string_view msg) {
        std::cout << msg;
    }
};


}
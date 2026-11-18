export module ylog:sink.stdout;

import std;
import :sink;

namespace ylog {

// 输出到 stdout。实现 Sink 概念。
export class StdoutSink {
public:
    auto write(std::string_view line) -> void {
        std::cout.write(line.data(), static_cast<std::streamsize>(line.size()));
    }

    auto flush() -> void {
        std::cout.flush();
    }
};

} // namespace ylog

export module ylog:sink.file;

import std;

namespace ylog {
export class SinkFile {
public:

	SinkFile(const std::string_view& filename){        
        stream_ = std::make_unique<std::ofstream>(filename);
        if (!stream_->is_open()) {
            auto err_msg = std::format("Failed to open file: {}", filename);
            throw std::ios_base::failure(err_msg);
        }
    }

    SinkFile(const SinkFile&) = delete;
    SinkFile& operator=(const SinkFile&) = delete;
    
    SinkFile(SinkFile&& other) noexcept {
        this->stream_ = std::move(other.stream_);
    }

    SinkFile& operator=(SinkFile&& other) noexcept {
        if (this != &other) { 
            SinkFile temp(std::move(other));
            *this = std::move(temp);  
        }
        return *this;
    }


	~SinkFile(){
        if(stream_){
            stream_->close();
        }
    }

	void write(std::string_view message){
        *stream_ << message;
    }

private:
    std::unique_ptr<std::ofstream> stream_ = nullptr;
};

}
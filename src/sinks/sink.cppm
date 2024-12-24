export module ylog:sink;

import std;


namespace ylog {

template <typename T>
concept Sink = requires(T t) {
    { t.write(std::string_view{}) } -> std::same_as<void>;
};


export class SinkIface {
public:
    template<Sink T>
    SinkIface(T&& sink): self_(std::make_unique<Model<T>>(std::forward<T>(sink))) {}
    
    // 禁止拷贝，但允许移动
    SinkIface(const SinkIface&) = delete;
    SinkIface& operator=(const SinkIface&) = delete;
    SinkIface(SinkIface&&) noexcept = default;
    SinkIface& operator=(SinkIface&&) noexcept = default;

    void write(std::string_view msg) {
        self_->write(msg);
    }

    template <typename T>
    T* as() {
        if (self_->type() == typeid(T)) {
            return &static_cast<Model<T>*>(self_.get())->sink_; // 转换为具体类型
        }
        return nullptr;
    }

private:
    // 动态分派接口
    struct Concept {
        virtual ~Concept() = default;
        virtual void write(std::string_view msg) = 0;
        virtual const std::type_info& type() const = 0; // 返回类型信息
    };

    template <Sink T>
    struct Model : Concept {        
        Model(T&& sink) : sink_(std::move(sink)) {}
        void write(std::string_view msg) override { sink_.write(msg); }
        const std::type_info& type() const override { return typeid(T); }
        T& get() { return sink_; } // 提供访问原始对象的方法

        T sink_;
    };

    std::unique_ptr<Concept> self_; 
};

}
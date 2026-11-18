# 静态反射仅用于 detail 内部

C++26 静态反射（GCC 的 `-freflection`）只用于 `ylog:message` 的 `detail` 实现，不进入公开接口。反射用于从类型生成字段信息，但公开接口只暴露 `Message` 与 `field_format` 等稳定形状；这样既利用了反射的便利，又避免让调用方与编译器对反射的差异化支持耦合。公开接口中的 `field_format<T>()` 对调用方返回 `std::string_view`（指向编译期生成的静态字符串），用于 splice 的反射信息只留在 `ylog:message` 的 `detail` 中，不出现在任何导出签名里。代价是部分字段信息需要在内部转换一次，收益是公开接口不依赖尚在演进的编译器实现。

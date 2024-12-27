module;

export module ylog;

export import :level;
export import :logger;  // 导出分区 logger

export import :sink;
export import :sink.file;
export import :sink.stdout;

import :message; // 分区 message

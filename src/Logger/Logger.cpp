//
// Created by lacas on 2026/3/10.
//
//
// Created by lacas on 2026/3/8.
//
#include <spdlog/spdlog.h>
#include "Logger.h"
#include "filesystem"
#include "spdlog/sinks/rotating_file_sink.h"
#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/async.h"

using namespace std;

Logger Logger::m_instance;

Logger &Logger::get_instance() {
    return m_instance;
}

void Logger::init(const std::string &loggerName, const std::string &filePath, size_t maxFileSize, size_t maxFiles,
                  Logger::Level level) {
    filesystem::path dir = std::filesystem::path(filePath).parent_path();
    if(!dir.empty()) std::filesystem::create_directories(dir);

    std::vector<spdlog::sink_ptr> sinks;

    auto console_sink = std::make_shared<spdlog::sinks::ansicolor_stdout_sink_mt>();
    console_sink->set_level(static_cast<spdlog::level::level_enum>(level));
    console_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%n] %v");
    m_instance.m_console_sink = console_sink;
    sinks.push_back(console_sink);

    auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(filePath, maxFileSize, maxFiles);
    file_sink->set_level(static_cast<spdlog::level::level_enum>(level));
    file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] [thread %t] %v");
    m_instance.m_file_sink = file_sink;
    sinks.push_back(file_sink);

    std::shared_ptr<spdlog::logger> logger;
    spdlog::init_thread_pool(1 << 13 , 1);
    logger = std::make_shared<spdlog::async_logger>(
            loggerName,
            sinks.begin(),sinks.end(),
            spdlog::thread_pool(),
            spdlog::async_overflow_policy::block);
    logger->set_level(static_cast<spdlog::level::level_enum>(level));
    logger->flush_on(spdlog::level::warn);

    spdlog::register_logger(logger);
    m_instance.m_logger = logger;
}

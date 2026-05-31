//
// Created by lacas on 2026/3/10.
//

#ifndef WEBPROJECT_LOGGER_H
#define WEBPROJECT_LOGGER_H
#include "spdlog/spdlog.h"

class Logger{
private:
    Logger(){init();};
    ~Logger() = default;

    static Logger m_instance;
    std::shared_ptr<spdlog::logger>        m_logger;
    std::shared_ptr<spdlog::sinks::sink>   m_console_sink;
    std::shared_ptr<spdlog::sinks::sink>   m_file_sink;
public:
    enum class Level {
        Trace   = spdlog::level::trace,
        Debug   = spdlog::level::debug,
        Info    = spdlog::level::info,
        Warn    = spdlog::level::warn,
        Error   = spdlog::level::err,
        Critical= spdlog::level::critical,
        Off     = spdlog::level::off
    };
public:
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    static Logger& get_instance();
public:
    static void init(const std::string& loggerName = "Test",
                     const std::string& filePath = "../logs/test.log",
                     size_t maxFileSize = (1 << 11) * 10,size_t maxFiles = 5,
                     Level level = Level::Debug);

    template<typename... Args>
    void Trace(fmt::format_string<Args...> fmt, Args&&... args) {
        if (m_logger) m_logger->trace(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
    template<typename... Args>
    void Debug(fmt::format_string<Args...> fmt, Args&&... args) {
        if (m_logger) m_logger->debug(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
    template<typename... Args>
    void Info(fmt::format_string<Args...> fmt, Args&&... args) noexcept {
        if (m_logger) m_logger->info(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
    template<typename... Args>
    void Warn(fmt::format_string<Args...> fmt, Args&&... args) {
        if (m_logger) m_logger->warn(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
    template<typename... Args>
    void Error(fmt::format_string<Args...> fmt, Args&&... args) {
        if (m_logger) m_logger->error(fmt::runtime(fmt), std::forward<Args>(args)...);
    }
    template<typename... Args>
    void Critical(fmt::format_string<Args...> fmt, Args&&... args) {
        if (m_logger) m_logger->critical(fmt::runtime(fmt), std::forward<Args>(args)...);
    }

};

#define LOGGER_INF(...) Logger::get_instance().Info(__VA_ARGS__)
#define LOGGER_WARN(...) Logger::get_instance().Warn(__VA_ARGS__)
#define LOGGER_TRACE(...) Logger::get_instance().Trace(__VA_ARGS__)
#define LOGGER_DEBUG(...) Logger::get_instance().Debug(__VA_ARGS__)
#define LOGGER_ERROR(...) Logger::get_instance().Error(__VA_ARGS__)
#define LOGGER_CRITICAL(...) Logger::get_instance().Critical(__VA_ARGS__)
#endif //WEBPROJECT_LOGGER_H

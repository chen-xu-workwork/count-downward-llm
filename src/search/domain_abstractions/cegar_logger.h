#ifndef DOMAIN_ABSTRACTIONS_CEGAR_LOGGER_H
#define DOMAIN_ABSTRACTIONS_CEGAR_LOGGER_H

#include <iostream>

namespace domain_abstractions {

// Verbosity levels for logging
enum class Verbosity {
    NONE,   // No output
    INFO,   // Important milestones and summaries
    DEBUG,   // Detailed debugging information
    VERBOSE
};

// Simple logger that respects verbosity levels
class CEGARLogger {
public:
    explicit CEGARLogger(Verbosity verbosity) : verbosity_level(verbosity) {}
    
    bool should_log(Verbosity level) const {
        return static_cast<int>(level) <= static_cast<int>(verbosity_level);
    }
    
    template<typename... Args>
    void log(Verbosity level, Args&&... args) const {
        if (should_log(level)) {
            (std::cout << ... << args) << std::endl;
        }
    }
    
    template<typename... Args>
    void log_no_endl(Verbosity level, Args&&... args) const {
        if (should_log(level)) {
            (std::cout << ... << args);
        }
    }
    
    Verbosity get_verbosity() const {
        return verbosity_level;
    }
    
private:
    Verbosity verbosity_level;
};

} // namespace domain_abstractions

#endif

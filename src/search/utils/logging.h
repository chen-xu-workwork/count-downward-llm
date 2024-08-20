#ifndef UTILS_LOGGING_H
#define UTILS_LOGGING_H

#include "system.h"
#include "timer.h"

#include <ostream>
#include <set>
#include <string>
#include <vector>

namespace utils {
/*
  Simple logger that prepends time and peak memory info to messages.
  Logs are written to stdout.

  Usage:
        g_log << "States: " << num_states << endl;
*/
struct Log {
    template<typename T>
    std::ostream &operator<<(const T &elem) {
        return std::cout << "[t=" << g_timer << ", "
                         << get_peak_memory_in_kb() << " KB] " << elem;
    }
};

class TraceBlock {
    std::string block_name;
public:
    explicit TraceBlock(const std::string &block_name);
    ~TraceBlock();
};

extern void trace(const std::string &msg = "");
}

namespace std {
template<class T>
ostream &operator<<(ostream &stream, const vector<T> &vec) {
    stream << "[";
    for (size_t i = 0; i < vec.size(); ++i) {
        if (i != 0)
            stream << ", ";
        stream << vec[i];
    }
    stream << "]";
    return stream;
}

template<class T>
ostream &operator<<(ostream &stream, const set<T> &s) {
    stream << "{";
    for (const T &e : s) {
        if (e != *s.begin())
            stream << ", ";
        stream << e;
    }
    stream << "}";
    return stream;
}

template<class S, class T>
ostream &operator<<(ostream &stream, const pair<S, T> &s) {
    stream << "(" << s.first << ", " << s.second << ")";
    return stream;
}
}

#endif

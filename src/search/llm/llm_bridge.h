#ifndef LLM_LLM_BRIDGE_H
#define LLM_LLM_BRIDGE_H

#include "llm_protocol.h"

#include <cstddef>
#include <memory>
#include <vector>

namespace llm {

class Bridge {
    class Impl;
    std::unique_ptr<Impl> impl;

public:
    Bridge();
    ~Bridge();

    Bridge(const Bridge &) = delete;
    Bridge &operator=(const Bridge &) = delete;

    void start();
    void stop();
    bool submit(const Request &request);
    bool expects_response() const;
    std::vector<Response> poll_completed();

    std::size_t queued_discarded() const;
    std::size_t active_cancelled() const;
    std::size_t completed_unconsumed() const;
};

}

#endif

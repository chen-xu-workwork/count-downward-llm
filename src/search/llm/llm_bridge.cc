#include "llm_bridge.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#if !defined(_WIN32)
#include <netdb.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using namespace std;

namespace llm {

class Bridge::Impl {
    struct Config {
        bool enabled;
        string mode;
        string host;
        int port;
        string path;
        int timeout_ms;
        int worker_count;
        int max_queue;
        bool emit_state;

        Config()
            : enabled(setting_enabled("TRIGGER")),
              mode(setting_string("COMM_MODE", "log")),
              host(setting_string("HTTP_HOST", "127.0.0.1")),
              port(setting_int("HTTP_PORT", 8765)),
              path(setting_string("HTTP_PATH", "/llm/request")),
              timeout_ms(max(1, setting_int("HTTP_TIMEOUT_MS", 30000))),
              worker_count(max(1, setting_int("HTTP_WORKERS", 8))),
              max_queue(setting_int("HTTP_MAX_QUEUE", 0)),
              emit_state(setting_enabled("EMIT_STATE")) {
            transform(mode.begin(), mode.end(), mode.begin(),
                      [](unsigned char ch) {
                          return static_cast<char>(tolower(ch));
                      });
        }
    };

    Config config;
    mutable mutex queue_mutex;
    condition_variable queue_cv;
    queue<Request> outgoing;
    deque<Response> completed;
    vector<thread> worker_threads;
    size_t active_requests;
    bool stopping;
    size_t queued_discarded_on_stop;
    size_t active_cancelled_on_stop;
    size_t completed_unconsumed_on_stop;
#if !defined(_WIN32)
    mutable mutex socket_mutex;
    unordered_set<int> active_sockets;
#endif

    bool http_mode() const {
        return config.enabled && config.mode == "http";
    }

    static string make_request_body(const Request &request) {
        ostringstream body;
        body << "{";
        body << "\"type\":\"llm_request\",";
        body << "\"request_id\":\"" << json_escape(request.request_id)
             << "\",";
        body << "\"run_id\":\"" << json_escape(request.run_id) << "\",";
        body << "\"iteration\":" << request.iteration << ",";
        body << "\"state_id\":" << request.state_index << ",";
        body << "\"state_label\":\"" << json_escape(request.state_label)
             << "\",";
        body << "\"problem_id\":\"" << json_escape(request.problem_id)
             << "\",";
        body << "\"reason\":\"" << json_escape(request.reason) << "\",";
        body << "\"g\":" << request.g << ",";
        body << "\"h\":" << request.h << ",";
        body << "\"search_expansions\":" << request.search_expansions
             << ",";
        body << "\"phase_elapsed_seconds\":"
             << request.phase_elapsed_seconds << ",";
        body << "\"init\":\"" << json_escape(request.init) << "\"";
        body << "}";
        return body.str();
    }

    string make_http_request(const string &body) const {
        ostringstream request;
        request << "POST " << config.path << " HTTP/1.1\r\n";
        request << "Host: " << config.host << ":" << config.port << "\r\n";
        request << "Content-Type: application/json\r\n";
        request << "Accept: application/json\r\n";
        request << "Connection: close\r\n";
        request << "Content-Length: " << body.size() << "\r\n\r\n";
        request << body;
        return request.str();
    }

    static int parse_http_status(const string &response) {
        if (response.size() < 12 || response.compare(0, 5, "HTTP/") != 0)
            return 0;
        size_t first_space = response.find(' ');
        return first_space == string::npos
            ? 0 : atoi(response.c_str() + first_space + 1);
    }

    static string parse_http_body(const string &response) {
        size_t split = response.find("\r\n\r\n");
        return split == string::npos ? "" : response.substr(split + 4);
    }

    static Response response_for(const Request &request) {
        Response response;
        response.request_id = request.request_id;
        response.run_id = request.run_id;
        response.iteration = request.iteration;
        response.state_id = request.state_id;
        response.state_index = request.state_index;
        response.state_label = request.state_label;
        return response;
    }

    void push_completed(const Response &response) {
        lock_guard<mutex> lock(queue_mutex);
        assert(active_requests > 0);
        --active_requests;
        completed.push_back(response);
    }

#if !defined(_WIN32)
    bool register_socket_if_running(int fd) {
        lock_guard<mutex> queue_lock(queue_mutex);
        if (stopping)
            return false;
        lock_guard<mutex> socket_lock(socket_mutex);
        active_sockets.insert(fd);
        return true;
    }

    void close_registered_socket(int fd) {
        lock_guard<mutex> lock(socket_mutex);
        active_sockets.erase(fd);
        close(fd);
    }

    bool send_all(int fd, const string &data, string &error) const {
        size_t sent = 0;
        while (sent < data.size()) {
            int flags = 0;
#if defined(MSG_NOSIGNAL)
            flags = MSG_NOSIGNAL;
#endif
            ssize_t count = send(
                fd, data.data() + sent, data.size() - sent, flags);
            if (count <= 0) {
                error = string("send failed: ") + strerror(errno);
                return false;
            }
            sent += static_cast<size_t>(count);
        }
        return true;
    }

    Response post_request_posix(const Request &request) {
        Response response = response_for(request);
        string port_string = to_string(config.port);
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;

        struct addrinfo *addresses = nullptr;
        int result = getaddrinfo(
            config.host.c_str(), port_string.c_str(), &hints, &addresses);
        if (result != 0) {
            response.error = string("getaddrinfo failed: ") + gai_strerror(result);
            return response;
        }

        int fd = -1;
        for (struct addrinfo *address = addresses;
             address; address = address->ai_next) {
            fd = socket(
                address->ai_family, address->ai_socktype, address->ai_protocol);
            if (fd == -1)
                continue;
            if (!register_socket_if_running(fd)) {
                close(fd);
                fd = -1;
                response.error = "bridge stopping";
                break;
            }

            struct timeval timeout;
            timeout.tv_sec = config.timeout_ms / 1000;
            timeout.tv_usec = (config.timeout_ms % 1000) * 1000;
            setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char *>(&timeout), sizeof(timeout));
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char *>(&timeout), sizeof(timeout));
            if (connect(fd, address->ai_addr, address->ai_addrlen) == 0)
                break;
            close_registered_socket(fd);
            fd = -1;
        }
        freeaddrinfo(addresses);

        if (fd == -1) {
            if (response.error.empty())
                response.error = "connect failed";
            return response;
        }

        string body = make_request_body(request);
        string send_error;
        if (!send_all(fd, make_http_request(body), send_error)) {
            response.error = send_error;
            close_registered_socket(fd);
            return response;
        }

        string raw_response;
        char buffer[4096];
        while (true) {
            ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
            if (count > 0) {
                raw_response.append(buffer, static_cast<size_t>(count));
            } else if (count == 0) {
                break;
            } else {
                response.error = string("recv failed: ") + strerror(errno);
                close_registered_socket(fd);
                return response;
            }
        }
        close_registered_socket(fd);

        response.http_status = parse_http_status(raw_response);
        response.body = parse_http_body(raw_response);
        response.transport_ok =
            response.http_status >= 200 && response.http_status < 300;
        if (!response.transport_ok && response.error.empty()) {
            ostringstream error;
            error << "http status " << response.http_status;
            response.error = error.str();
        }
        return response;
    }
#else
    Response post_request_windows_stub(const Request &request) const {
        Response response = response_for(request);
        response.error = "HTTP bridge is only implemented for POSIX/WSL builds";
        return response;
    }
#endif

    Response post_request(const Request &request) {
#if !defined(_WIN32)
        return post_request_posix(request);
#else
        return post_request_windows_stub(request);
#endif
    }

    void worker_loop(int worker_index) {
        while (true) {
            Request request;
            size_t in_flight;
            size_t queued;
            {
                unique_lock<mutex> lock(queue_mutex);
                queue_cv.wait(lock, [this]() {
                    return stopping || !outgoing.empty();
                });
                if (stopping)
                    return;
                request = outgoing.front();
                outgoing.pop();
                ++active_requests;
                in_flight = active_requests;
                queued = outgoing.size();
            }
            cout << "[HYBRID-LLM-BRIDGE] dispatched"
                 << " worker=" << worker_index
                 << " request_id=" << request.request_id
                 << " state=" << request.state_label
                 << " in_flight=" << in_flight
                 << " queued=" << queued << endl;
            push_completed(post_request(request));
        }
    }

    void log_request(const Request &request) const {
        cout << "[NLM-LLM-TRIGGER] request"
             << " run_id=" << request.run_id
             << " iteration=" << request.iteration
             << " request_id=" << request.request_id
             << " state=" << request.state_label
             << " reason=" << request.reason
             << " g=" << request.g
             << " h=" << request.h
             << " expansions=" << request.search_expansions
             << " phase_elapsed_seconds=" << request.phase_elapsed_seconds
             << " comm_mode=" << config.mode << endl;
        if (config.emit_state) {
            cout << "[NLM-LLM-TRIGGER-STATE] begin state="
                 << request.state_label << endl;
            cout << request.init;
            cout << "[NLM-LLM-TRIGGER-STATE] end state="
                 << request.state_label << endl;
        }
    }

public:
    Impl()
        : active_requests(0),
          stopping(false),
          queued_discarded_on_stop(0),
          active_cancelled_on_stop(0),
          completed_unconsumed_on_stop(0) {
    }

    ~Impl() {
        stop();
    }

    void start() {
        if (!config.enabled)
            return;
        if (http_mode()) {
            cout << "[HYBRID-LLM-BRIDGE] starting http worker pool"
                 << " host=" << config.host
                 << " port=" << config.port
                 << " path=" << config.path
                 << " timeout_ms=" << config.timeout_ms
                 << " workers=" << config.worker_count
                 << " max_queue=" << config.max_queue << endl;
            worker_threads.reserve(config.worker_count);
            for (int index = 0; index < config.worker_count; ++index)
                worker_threads.emplace_back(&Impl::worker_loop, this, index);
        } else {
            cout << "[HYBRID-LLM-BRIDGE] using log mode"
                 << " comm_mode=" << config.mode << endl;
        }
    }

    void stop() {
        {
            lock_guard<mutex> lock(queue_mutex);
            if (stopping)
                return;
            stopping = true;
            queued_discarded_on_stop += outgoing.size();
            active_cancelled_on_stop += active_requests;
            completed_unconsumed_on_stop += completed.size();
            queue<Request> empty;
            outgoing.swap(empty);
        }
        queue_cv.notify_all();
#if !defined(_WIN32)
        {
            lock_guard<mutex> lock(socket_mutex);
            for (int fd : active_sockets)
                shutdown(fd, SHUT_RDWR);
        }
#endif
        for (thread &worker : worker_threads) {
            if (worker.joinable())
                worker.join();
        }
        if (!worker_threads.empty()) {
            cout << "[HYBRID-LLM-BRIDGE] http worker pool stopped"
                 << " workers=" << worker_threads.size() << endl;
            worker_threads.clear();
        }
    }

    bool submit(const Request &request) {
        if (!config.enabled)
            return false;
        if (!http_mode()) {
            log_request(request);
            return true;
        }
        {
            lock_guard<mutex> lock(queue_mutex);
            if (stopping)
                return false;
            if (config.max_queue > 0 &&
                static_cast<int>(outgoing.size()) >= config.max_queue) {
                return false;
            }
            outgoing.push(request);
        }
        queue_cv.notify_one();
        cout << "[HYBRID-LLM-BRIDGE] submitted"
             << " request_id=" << request.request_id
             << " state=" << request.state_label
             << " reason=" << request.reason << endl;
        return true;
    }

    bool expects_response() const {
        return http_mode();
    }

    vector<Response> poll_completed() {
        vector<Response> responses;
        lock_guard<mutex> lock(queue_mutex);
        while (!completed.empty()) {
            responses.push_back(completed.front());
            completed.pop_front();
        }
        return responses;
    }

    size_t queued_discarded() const {
        lock_guard<mutex> lock(queue_mutex);
        return queued_discarded_on_stop;
    }
    size_t active_cancelled() const {
        lock_guard<mutex> lock(queue_mutex);
        return active_cancelled_on_stop;
    }
    size_t completed_unconsumed() const {
        lock_guard<mutex> lock(queue_mutex);
        return completed_unconsumed_on_stop;
    }
};

Bridge::Bridge()
    : impl(new Impl()) {
}

Bridge::~Bridge() = default;

void Bridge::start() {
    impl->start();
}

void Bridge::stop() {
    impl->stop();
}

bool Bridge::submit(const Request &request) {
    return impl->submit(request);
}

bool Bridge::expects_response() const {
    return impl->expects_response();
}

vector<Response> Bridge::poll_completed() {
    return impl->poll_completed();
}

size_t Bridge::queued_discarded() const {
    return impl->queued_discarded();
}

size_t Bridge::active_cancelled() const {
    return impl->active_cancelled();
}

size_t Bridge::completed_unconsumed() const {
    return impl->completed_unconsumed();
}

}

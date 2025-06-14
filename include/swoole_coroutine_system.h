/*
  +----------------------------------------------------------------------+
  | Swoole                                                               |
  +----------------------------------------------------------------------+
  | This source file is subject to version 2.0 of the Apache license,    |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.apache.org/licenses/LICENSE-2.0.html                      |
  | If you did not receive a copy of the Apache2.0 license and are unable|
  | to obtain it through the world-wide-web, please send a note to       |
  | license@swoole.com so we can mail you a copy immediately.            |
  +----------------------------------------------------------------------+
  | Author: Tianfeng Han  <rango@swoole.com>                             |
  |         Twosee  <twose@qq.com>                                       |
  +----------------------------------------------------------------------+
*/

#pragma once

#include "swoole_coroutine.h"

#include <vector>

namespace swoole {
namespace coroutine {
//-------------------------------------------------------------------------------
struct PollSocket {
    int events;
    int16_t revents;
    void *ptr;
    network::Socket *socket;

    PollSocket(int _event, void *_ptr) {
        events = _event;
        ptr = _ptr;
        revents = 0;
        socket = nullptr;
    }
};

class System {
  public:
    static void init_reactor(Reactor *reactor);
    /* sleep */
    static int sleep(double sec);
    /* file */
    static std::shared_ptr<String> read_file(const char *file, bool lock = false);
    static ssize_t write_file(const char *file, const char *buf, size_t length, bool lock = 0, int flags = 0);
    /* dns */
    static std::string gethostbyname(const std::string &hostname, int domain, double timeout = -1);
    static std::vector<std::string> getaddrinfo(const std::string &hostname,
                                                int family = AF_INET,
                                                int socktype = SOCK_STREAM,
                                                int protocol = IPPROTO_TCP,
                                                const std::string &service = "",
                                                double timeout = -1);
    static void set_dns_cache_expire(time_t expire);
    static void set_dns_cache_capacity(size_t capacity);
    static void clear_dns_cache();
    static float get_dns_cache_hit_ratio();
    /* multiplexing */
    static bool socket_poll(std::unordered_map<int, PollSocket> &fds, double timeout);
    /* wait */
    static pid_t wait(int *__stat_loc, double timeout = -1);
    static pid_t waitpid(pid_t __pid, int *__stat_loc, int __options, double timeout = -1);
    /**
     * waitpid_safe() does not deps on the signal
     * and can be safely used in a multi-threaded environment.
     */
    static pid_t waitpid_safe(pid_t __pid, int *__stat_loc, int __options);
    /* signal */
    static int wait_signal(int signal, double timeout = -1);
    static int wait_signal(const std::vector<int> &signals, double timeout = -1);
    /* event */

    /**
     * On failure, it returns -1, and you can use swoole_get_last_error() or errno to determine the cause of the
     * failure. If successful, it returns the event set, such as SW_EVENT_READ | SW_EVENT_WRITE.
     */
    static int wait_event(int fd, int events, double timeout);
    static bool exec(const char *command, bool get_error_stream, std::shared_ptr<String> buffer, int *status);
};
std::string gethostbyname_impl_with_async(const std::string &hostname, int domain, double timeout = -1);
//-------------------------------------------------------------------------------
}  // namespace coroutine
}  // namespace swoole

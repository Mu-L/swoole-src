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
  +----------------------------------------------------------------------+
*/

#include "swoole_timer.h"
#include "swoole_signal.h"

#include <csignal>

namespace swoole {
static int SystemTimer_set(Timer *timer, long next_msec);

void Timer::init_with_system_timer() {
    set = SystemTimer_set;
    close = [](Timer *timer) { SystemTimer_set(timer, -1); };
    swoole_signal_set(SIGALRM, [](int sig) { SwooleG.signal_alarm = true; });
}

static int SystemTimer_set(Timer *timer, long next_msec) {
    itimerval timer_set;
    timeval now;
    if (gettimeofday(&now, nullptr) < 0) {
        swoole_sys_warning("gettimeofday() failed");
        return SW_ERR;
    }

    if (next_msec > 0) {
        int sec = next_msec / 1000;
        int msec = next_msec % 1000;
        timer_set.it_interval.tv_sec = sec;
        timer_set.it_interval.tv_usec = msec * 1000;
        timer_set.it_value.tv_sec = sec;
        timer_set.it_value.tv_usec = timer_set.it_interval.tv_usec;

        if (timer_set.it_value.tv_usec > 1e6) {
            timer_set.it_value.tv_usec = timer_set.it_value.tv_usec - 1e6;
            timer_set.it_value.tv_sec += 1;
        }
    } else {
        timer_set = {};
    }

    if (setitimer(ITIMER_REAL, &timer_set, nullptr) < 0) {
        swoole_sys_warning("setitimer() failed");
        return SW_ERR;
    }
    return SW_OK;
}

void realtime_get(timespec *time) {
    auto now = std::chrono::system_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch());
    time->tv_sec = ns.count() / SW_NUM_BILLION;
    time->tv_nsec = ns.count() % SW_NUM_BILLION;
}

void realtime_add(timespec *time, const int64_t add_msec) {
    time->tv_sec += add_msec / 1000;
    time->tv_nsec += add_msec % 1000 * SW_NUM_MILLION;
}
}  // namespace swoole

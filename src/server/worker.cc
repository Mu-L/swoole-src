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

#include <sys/uio.h>
#include <sys/mman.h>

#include "swoole_server.h"
#include "swoole_memory.h"
#include "swoole_coroutine.h"

namespace swoole {
using namespace network;

static int Worker_onPipeReceive(Reactor *reactor, Event *event);
static void Worker_reactor_try_to_exit(Reactor *reactor);

static void Worker_reopen_logger() {
    if (sw_logger()) {
        sw_logger()->reopen();
    }
}

void Server::worker_signal_init() const {
    if (is_thread_mode()) {
        return;
    }
    swoole_signal_set(SIGHUP, nullptr);
    swoole_signal_set(SIGPIPE, SIG_IGN);
    swoole_signal_set(SIGUSR1, nullptr);
    swoole_signal_set(SIGUSR2, nullptr);
    swoole_signal_set(SIGTERM, worker_signal_handler);
    swoole_signal_set(SIGWINCH, worker_signal_handler);
#ifdef SIGRTMIN
    swoole_signal_set(SIGRTMIN, worker_signal_handler);
#endif
}

void Server::worker_signal_handler(int signo) {
    if (!SwooleG.running || !sw_server() || !sw_worker() || !sw_server()->is_running()) {
        return;
    }
    switch (signo) {
    case SIGTERM:
        if (swoole_event_is_available()) {
            sw_server()->stop_async_worker(sw_worker());
        } else {
            sw_worker()->shutdown();
        }
        break;
    case SIGWINCH:
        Worker_reopen_logger();
        break;
    default:
#ifdef SIGRTMIN
        if (signo == SIGRTMIN) {
            Worker_reopen_logger();
        }
#endif
        break;
    }
}

static sw_inline bool Worker_discard_data(const Server *serv, const Connection *conn, const DataHead *info) {
    if (conn == nullptr) {
        if (serv->disable_notify && !serv->discard_timeout_request) {
            return false;
        }
        goto _discard_data;
    } else {
        if (conn->closed) {
            goto _discard_data;
        } else {
            return false;
        }
    }
_discard_data:
    swoole_error_log(SW_LOG_WARNING,
                     SW_ERROR_SESSION_DISCARD_TIMEOUT_DATA,
                     "[2] ignore data[%u bytes] received from session#%ld",
                     info->len,
                     info->fd);
    return true;
}

typedef std::function<int(Server *, RecvData *)> TaskCallback;

static sw_inline void Worker_do_task(Server *serv, Worker *worker, const DataHead *info, const TaskCallback &callback) {
    RecvData recv_data;
    auto packet = serv->get_worker_message_bus()->get_packet();
    recv_data.info = *info;
    recv_data.info.len = packet.length;
    recv_data.data = packet.data;

    if (callback(serv, &recv_data) == SW_OK) {
        worker->add_request_count();
        sw_atomic_fetch_add(&serv->gs->request_count, 1);
    }
}

void Server::worker_accept_event(DataHead *info) {
    Worker *worker = sw_worker();
    worker->set_status_to_busy();

    switch (info->type) {
    case SW_SERVER_EVENT_RECV_DATA: {
        Connection *conn = get_connection_verify(info->fd);
        if (conn) {
            if (info->len > 0) {
                auto packet = get_worker_message_bus()->get_packet();
                sw_atomic_fetch_sub(&conn->recv_queued_bytes, packet.length);
                swoole_trace_log(SW_TRACE_SERVER,
                                 "[Worker] session_id=%ld, len=%lu, qb=%d",
                                 conn->session_id,
                                 packet.length,
                                 conn->recv_queued_bytes);
            }
            conn->last_dispatch_time = info->time;
        }
        if (!Worker_discard_data(this, conn, info)) {
            Worker_do_task(this, worker, info, onReceive);
        }
        break;
    }
    case SW_SERVER_EVENT_RECV_DGRAM: {
        Worker_do_task(this, worker, info, onPacket);
        break;
    }
    case SW_SERVER_EVENT_CLOSE: {
#ifdef SW_USE_OPENSSL
        Connection *conn = get_connection_verify_no_ssl(info->fd);
        if (conn && conn->ssl_client_cert && conn->ssl_client_cert_pid == swoole_get_worker_pid()) {
            delete conn->ssl_client_cert;
            conn->ssl_client_cert = nullptr;
        }
#endif
        factory->end(info->fd, false);
        break;
    }
    case SW_SERVER_EVENT_CONNECT: {
#ifdef SW_USE_OPENSSL
        // SSL client certificate
        if (info->len > 0) {
            Connection *conn = get_connection_verify_no_ssl(info->fd);
            if (conn) {
                auto packet = get_worker_message_bus()->get_packet();
                conn->ssl_client_cert = new String(packet.data, packet.length);
                conn->ssl_client_cert_pid = swoole_get_worker_pid();
            }
        }
#endif
        if (onConnect) {
            onConnect(this, info);
        }
        break;
    }

    case SW_SERVER_EVENT_BUFFER_FULL: {
        if (onBufferFull) {
            onBufferFull(this, info);
        }
        break;
    }
    case SW_SERVER_EVENT_BUFFER_EMPTY: {
        if (onBufferEmpty) {
            onBufferEmpty(this, info);
        }
        break;
    }
    case SW_SERVER_EVENT_FINISH: {
        onFinish(this, reinterpret_cast<EventData *>(get_worker_message_bus()->get_buffer()));
        break;
    }
    case SW_SERVER_EVENT_PIPE_MESSAGE: {
        onPipeMessage(this, reinterpret_cast<EventData *>(get_worker_message_bus()->get_buffer()));
        break;
    }
    case SW_SERVER_EVENT_COMMAND_REQUEST: {
        call_command_handler(message_bus, worker->id, pipe_command->get_socket(false));
        break;
    }
    case SW_SERVER_EVENT_SHUTDOWN: {
        stop_async_worker(worker);
        break;
    }
    default:
        swoole_warning("[Worker] error event[type=%d]", (int) info->type);
        break;
    }

    worker->set_status_to_idle();

    // maximum number of requests, process will exit.
    if (worker->has_exceeded_max_request()) {
        if (is_thread_mode()) {
            Reactor *reactor = sw_reactor();
            get_thread(reactor->id)->shutdown(reactor);
        } else {
            stop_async_worker(worker);
        }
    }
}

static bool is_root_user() {
    return geteuid() == 0;
}

void Server::worker_start_callback(Worker *worker) {
    if (is_root_user()) {
        Worker::set_isolation(group_, user_, chroot_);
    }

    SW_LOOP_N(worker_num + task_worker_num) {
        if (worker->id == i) {
            continue;
        }
        Worker *other_worker = get_worker(i);
        if (is_worker() && other_worker->pipe_master) {
            other_worker->pipe_master->set_nonblock();
        }
    }

    worker->set_status_to_idle();

    if (is_process_mode()) {
        sw_shm_protect(session_list, PROT_READ);
    }

    call_worker_start_callback(worker);
}

void Server::worker_stop_callback(Worker *worker) {
    call_worker_stop_callback(worker);
}

void Server::call_worker_start_callback(Worker *worker) {
    void *hook_args[2];
    hook_args[0] = this;
    hook_args[1] = (void *) (uintptr_t) worker->id;

    if (swoole_isset_hook(SW_GLOBAL_HOOK_BEFORE_WORKER_START)) {
        swoole_call_hook(SW_GLOBAL_HOOK_BEFORE_WORKER_START, hook_args);
    }
    if (isset_hook(HOOK_WORKER_START)) {
        call_hook(HOOK_WORKER_START, hook_args);
    }

    swoole_clear_last_error();
    swoole_clear_last_error_msg();

    if (onWorkerStart) {
        onWorkerStart(this, worker);
    }
}

void Server::call_worker_stop_callback(Worker *worker) {
    void *hook_args[2];
    hook_args[0] = this;
    hook_args[1] = (void *) (uintptr_t) worker->id;

    if (swoole_isset_hook(SW_GLOBAL_HOOK_BEFORE_WORKER_STOP)) {
        swoole_call_hook(SW_GLOBAL_HOOK_BEFORE_WORKER_STOP, hook_args);
    }
    if (onWorkerStop) {
        onWorkerStop(this, worker);
    }

    if (!get_worker_message_bus()->empty()) {
        swoole_error_log(
            SW_LOG_WARNING, SW_ERROR_SERVER_WORKER_UNPROCESSED_DATA, "unprocessed data in the worker process buffer");
        get_worker_message_bus()->clear();
    }

    SwooleWG.running = false;
    if (SwooleWG.worker_copy) {
        delete SwooleWG.worker_copy;
        SwooleWG.worker_copy = nullptr;
        SwooleWG.worker = nullptr;
    }
}

void Server::call_worker_error_callback(Worker *worker, const ExitStatus &status) {
    if (onWorkerError != nullptr) {
        onWorkerError(this, worker, status);
    }
    /**
     * The work process has exited unexpectedly, requiring a cleanup of the shared memory state.
     * This must be done between the termination of the old process and the initiation of the new one;
     * otherwise, data contention may occur.
     */
    if (worker->type == SW_EVENT_WORKER) {
        abort_worker(worker);
    }
}

bool Server::kill_worker(int worker_id) {
    auto current_worker = sw_worker();
    if (!current_worker && worker_id < 0) {
        swoole_error_log(
            SW_LOG_WARNING, SW_ERROR_WRONG_OPERATION, "kill worker in non worker process requires specifying an id");
        return false;
    }

    worker_id = worker_id < 0 ? swoole_get_worker_id() : worker_id;
    const Worker *worker = get_worker(worker_id);
    if (worker == nullptr) {
        swoole_error_log(SW_LOG_WARNING, SW_ERROR_INVALID_PARAMS, "the worker_id[%d] is invalid", worker_id);
        return false;
    }

    swoole_trace_log(SW_TRACE_SERVER, "kill worker#%d", worker_id);

    DataHead event = {};
    event.type = SW_SERVER_EVENT_SHUTDOWN;
    return send_to_worker_from_worker(worker, &event, sizeof(event), SW_PIPE_MASTER) != -1;
}

void Server::stop_async_worker(Worker *worker) {
    worker->shutdown();
    if (worker->type == SW_EVENT_WORKER) {
        reset_worker_counter(worker);
    }

    // forced termination
    Reactor *reactor = sw_reactor();
    if (reload_async == 0) {
        reactor->running = false;
        return;
    }

    // The worker process is shutting down now.
    if (reactor->wait_exit) {
        return;
    }

    // Separated from the event worker process pool
    SwooleWG.worker_copy = new Worker{};
    *SwooleWG.worker_copy = *worker;
    SwooleWG.worker = worker;
    auto pipe_worker = get_worker_pipe_worker_in_message_bus(worker);

    if (pipe_worker && !pipe_worker->removed) {
        reactor->remove_read_event(pipe_worker);
    }

    if (is_base_mode()) {
        if (is_event_worker()) {
            if (worker->id == 0 && get_event_worker_pool()->running == 0) {
                if (swoole_isset_hook(SW_GLOBAL_HOOK_BEFORE_SERVER_SHUTDOWN)) {
                    swoole_call_hook(SW_GLOBAL_HOOK_BEFORE_SERVER_SHUTDOWN, this);
                }
                if (onBeforeShutdown) {
                    onBeforeShutdown(this);
                }
            }
            if (worker->pipe_master && !worker->pipe_master->removed) {
                reactor->remove_read_event(worker->pipe_master);
            }
            for (auto ls : ports) {
                reactor->del(ls->socket);
            }
            foreach_connection([reactor](Connection *conn) {
                if (!conn->peer_closed && !conn->socket->removed) {
                    reactor->remove_read_event(conn->socket);
                }
            });
            clear_timer();
        }
    } else if (is_process_mode()) {
        WorkerStopMessage msg;
        msg.pid = getpid();
        msg.worker_id = worker->id;

        if (get_event_worker_pool()->push_message(SW_WORKER_MESSAGE_STOP, &msg, sizeof(msg)) < 0) {
            swoole_sys_warning("failed to push WORKER_STOP message");
        }
    } else if (is_thread_mode()) {
        if (is_event_worker()) {
            /**
             * The thread mode will use the master pipe to forward messages,
             * and it may listen for writable events on this pipe,
             * which need to be removed before the worker thread exits.
             */
            SW_LOOP_N(worker_num) {
                if (i % reactor_num == reactor->id) {
                    auto pipe_master = get_worker_pipe_master_in_message_bus(i);
                    if (!pipe_master->removed) {
                        reactor->remove_read_event(pipe_master);
                    }
                }
            }
            /**
             * Only the readable events are removed;
             * at this point, there may still be ongoing events for sending data.
             * The connection will be completely closed only when the reactor is destroyed.
             */
            foreach_connection([reactor](Connection *conn) {
                if (conn->reactor_id == reactor->id && !conn->peer_closed && !conn->socket->removed) {
                    reactor->remove_read_event(conn->socket);
                }
            });
        }
    } else {
        assert(0);
    }

    reactor->set_wait_exit(true);
    reactor->set_end_callback(Reactor::PRIORITY_TRY_EXIT, Worker_reactor_try_to_exit);
    SwooleWG.exit_time = ::time(nullptr);

    Worker_reactor_try_to_exit(reactor);
}

static void Worker_reactor_try_to_exit(Reactor *reactor) {
    Server *serv;
    if (sw_likely(swoole_get_worker_type() != SW_TASK_WORKER)) {
        serv = static_cast<Server *>(reactor->ptr);
    } else {
        auto pool = static_cast<ProcessPool *>(reactor->ptr);
        serv = static_cast<Server *>(pool->ptr);
    }

    bool has_call_worker_exit_func = false;
    while (true) {
        if (reactor->if_exit()) {
            reactor->running = false;
        } else {
            if (serv->onWorkerExit && !has_call_worker_exit_func) {
                has_call_worker_exit_func = true;
                serv->onWorkerExit(serv, sw_worker());
                continue;
            }
            int remaining_time = serv->max_wait_time - (::time(nullptr) - SwooleWG.exit_time);
            if (remaining_time <= 0) {
                swoole_error_log(
                    SW_LOG_WARNING, SW_ERROR_SERVER_WORKER_EXIT_TIMEOUT, "worker exit timeout, forced termination");
                reactor->running = false;
            } else {
                int timeout_msec = remaining_time * 1000;
                if (reactor->timeout_msec < 0 || reactor->timeout_msec > timeout_msec) {
                    reactor->timeout_msec = timeout_msec;
                }
            }
        }
        break;
    }
}

void Server::drain_worker_pipe() {
    for (uint32_t i = 0; i < worker_num + task_worker_num; i++) {
        Worker *worker = get_worker(i);
        if (sw_reactor()) {
            if (worker->pipe_worker) {
                sw_reactor()->drain_write_buffer(worker->pipe_worker);
            }
            if (worker->pipe_master) {
                sw_reactor()->drain_write_buffer(worker->pipe_master);
            }
        }
    }
}

void Server::clean_worker_connections(Worker *worker) {
    swoole_trace_log(SW_TRACE_WORKER, "clean connections");
    sw_reactor()->destroyed = true;
    if (sw_likely(is_base_mode())) {
        foreach_connection([this](Connection *conn) { close(conn->session_id, true); });
    } else if (is_thread_mode()) {
        foreach_connection([this, worker](Connection *conn) {
            if (conn->reactor_id == worker->id) {
                close(conn->session_id, true);
            }
        });
    }
}

/**
 * main loop [Worker]
 * Only used in SWOOLE_PROCESS mode
 */
int Server::start_event_worker(Worker *worker) {
    swoole_set_worker_id(worker->id);
    swoole_set_worker_type(SW_EVENT_WORKER);

    init_event_worker(worker);

    if (swoole_event_init(0) < 0) {
        return SW_ERR;
    }

    worker_signal_init();

    Reactor *reactor = SwooleTG.reactor;
    /**
     * set pipe buffer size
     */
    for (uint32_t i = 0; i < worker_num + task_worker_num; i++) {
        const Worker *_worker = get_worker(i);
        if (_worker->pipe_master) {
            _worker->pipe_master->buffer_size = UINT_MAX;
        }
        if (_worker->pipe_worker) {
            _worker->pipe_worker->buffer_size = UINT_MAX;
        }
    }

    worker->pipe_worker->set_nonblock();
    reactor->ptr = this;
    reactor->add(worker->pipe_worker, SW_EVENT_READ);
    reactor->set_handler(SW_FD_PIPE, SW_EVENT_READ, Worker_onPipeReceive);

    if (dispatch_mode == DISPATCH_CO_CONN_LB || dispatch_mode == DISPATCH_CO_REQ_LB) {
        reactor->set_end_callback(Reactor::PRIORITY_WORKER_CALLBACK,
                                  [worker](Reactor *) { worker->coroutine_num = Coroutine::count(); });
    }

    worker_start_callback(worker);

    // main loop
    const auto rv = reactor->wait();
    // drain pipe buffer
    drain_worker_pipe();
    // reactor free
    swoole_event_free();
    // worker shutdown
    worker_stop_callback(worker);

    delete buffer_pool;

    return rv;
}

/**
 * [Worker/TaskWorker/Master] Send data to ReactorThread
 */
ssize_t Server::send_to_reactor_thread(const EventData *ev_data, size_t sendn, SessionId session_id) {
    Socket *pipe_sock = get_reactor_pipe_socket(session_id, ev_data->info.reactor_id);
    if (swoole_event_is_available()) {
        return swoole_event_write(pipe_sock, ev_data, sendn);
    } else {
        return pipe_sock->send_sync(ev_data, sendn);
    }
}

/**
 * send message from worker to another worker
 */
ssize_t Server::send_to_worker_from_worker(const Worker *dst_worker, const void *buf, size_t len, int flags) {
    return dst_worker->send_pipe_message(buf, len, flags);
}

/**
 * receive data from reactor
 * This function is intended solely for process mode; in thread or base mode, `ReactorThread_onRead()` will be executed.
 */
static int Worker_onPipeReceive(Reactor *reactor, Event *event) {
    auto *serv = static_cast<Server *>(reactor->ptr);
    auto *pipe_buffer = serv->get_worker_message_bus()->get_buffer();

    if (serv->get_worker_message_bus()->read(event->socket) <= 0) {
        return SW_OK;
    }

    serv->worker_accept_event(&pipe_buffer->info);
    serv->get_worker_message_bus()->pop();

    return SW_OK;
}
}  // namespace swoole

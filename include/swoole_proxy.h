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

#pragma once

#include <string>
#include <cstdint>
#include <functional>

#define SW_SOCKS5_VERSION_CODE 0x05
#define SW_HTTP_PROXY_CHECK_MESSAGE 0
#define SW_HTTP_PROXY_HANDSHAKE_RESPONSE "HTTP/1.1 200 Connection established\r\n"

#define SW_HTTP_PROXY_FMT                                                                                              \
    "CONNECT %.*s:%d HTTP/1.1\r\n"                                                                                     \
    "Host: %.*s:%d\r\n"                                                                                                \
    "User-Agent: Swoole/" SWOOLE_VERSION "\r\n"                                                                        \
    "Proxy-Connection: Keep-Alive\r\n"

enum swHttpProxyState {
    SW_HTTP_PROXY_STATE_WAIT = 0,
    SW_HTTP_PROXY_STATE_HANDSHAKE,
    SW_HTTP_PROXY_STATE_READY,
};

enum swSocks5State {
    SW_SOCKS5_STATE_WAIT = 0,
    SW_SOCKS5_STATE_HANDSHAKE,
    SW_SOCKS5_STATE_AUTH,
    SW_SOCKS5_STATE_CONNECT,
    SW_SOCKS5_STATE_READY,
};

enum swSocks5Method {
    SW_SOCKS5_METHOD_NO_AUTH = 0x00,
    SW_SOCKS5_METHOD_AUTH = 0x02,
};

namespace swoole {
class String;

struct HttpProxy {
    uint8_t state;
    uint8_t dont_handshake;
    int port;
    std::string host;
    std::string username;
    std::string password;
    std::string target_host;
    int target_port;

    std::string get_auth_str();
    size_t pack(String *send_buffer, const std::string &host_name);
    bool handshake(String *recv_buffer);

    static HttpProxy *create(const std::string &host, int port, const std::string &user, const std::string &pwd);
};

struct Socks5Proxy {
    std::string host;
    int port;
    uint8_t state;
    uint8_t version;
    uint8_t method;
    uint8_t dns_tunnel;
    std::string username;
    std::string password;
    std::string target_host;
    int target_port;
    int socket_type;
    char buf[512];

    ssize_t pack_negotiate_request();
    ssize_t pack_auth_request();
    ssize_t pack_connect_request();
    bool handshake(const char *rbuf, size_t rlen, const std::function<ssize_t(const char *buf, size_t len)> &send_fn);

    static const char *strerror(int code);
    static Socks5Proxy *create(
        int socket_type, const std::string &host, int port, const std::string &user, const std::string &pwd);
};
}  // namespace swoole

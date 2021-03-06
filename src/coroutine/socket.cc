#include "Socket.h"
#include "context.h"
#include "async.h"
#include "buffer.h"

#include <string>
#include <iostream>
#include <sys/stat.h>

using namespace swoole;
using namespace std;

static int socket_onRead(swReactor *reactor, swEvent *event);
static int socket_onWrite(swReactor *reactor, swEvent *event);
static void socket_onTimeout(swTimer *timer, swTimer_node *tnode);
static void socket_onResolveCompleted(swAio_event *event);

bool Socket::socks5_handshake()
{
    swSocks5 *ctx = socks5_proxy;
    char *buf = ctx->buf;
    int n;

    /**
     * handshake
     */
    swSocks5_pack(buf, socks5_proxy->username == NULL ? 0x00 : 0x02);
    socks5_proxy->state = SW_SOCKS5_STATE_HANDSHAKE;
    if (send(buf, 3) <= 0)
    {
        return false;
    }
    n = recv(buf, sizeof(ctx->buf));
    if (n <= 0)
    {
        return false;
    }
    uchar version = buf[0];
    uchar method = buf[1];
    if (version != SW_SOCKS5_VERSION_CODE)
    {
        swoole_error_log(SW_LOG_NOTICE, SW_ERROR_SOCKS5_UNSUPPORT_VERSION, "SOCKS version is not supported.");
        return SW_ERR;
    }
    if (method != ctx->method)
    {
        swoole_error_log(SW_LOG_NOTICE, SW_ERROR_SOCKS5_UNSUPPORT_METHOD, "SOCKS authentication method not supported.");
        return SW_ERR;
    }
    //authenticate request
    if (method == SW_SOCKS5_METHOD_AUTH)
    {
        buf[0] = 0x01;
        buf[1] = ctx->l_username;

        buf += 2;
        memcpy(buf, ctx->username, ctx->l_username);
        buf += ctx->l_username;
        buf[0] = ctx->l_password;
        memcpy(buf + 1, ctx->password, ctx->l_password);

        ctx->state = SW_SOCKS5_STATE_AUTH;

        if (send(ctx->buf, ctx->l_username + ctx->l_password + 3) < 0)
        {
            return false;
        }

        n = recv(buf, sizeof(ctx->buf));
        if (n <= 0)
        {
            return false;
        }

        uchar version = buf[0];
        uchar status = buf[1];
        if (version != 0x01)
        {
            swoole_error_log(SW_LOG_NOTICE, SW_ERROR_SOCKS5_UNSUPPORT_VERSION, "SOCKS version is not supported.");
            return false;
        }
        if (status != 0)
        {
            swoole_error_log(SW_LOG_NOTICE, SW_ERROR_SOCKS5_AUTH_FAILED,
                    "SOCKS username/password authentication failed.");
            return false;
        }
        goto send_connect_request;
    }
    //send connect request
    else
    {
        send_connect_request: buf[0] = SW_SOCKS5_VERSION_CODE;
        buf[1] = 0x01;
        buf[2] = 0x00;

        ctx->state = SW_SOCKS5_STATE_CONNECT;

        if (ctx->dns_tunnel)
        {
            buf[3] = 0x03;
            buf[4] = ctx->l_target_host;
            buf += 5;
            memcpy(buf, ctx->target_host, ctx->l_target_host);
            buf += ctx->l_target_host;
            *(uint16_t *) buf = htons(ctx->target_port);

            if (send(ctx->buf, ctx->l_target_host + 7) < 0)
            {
                return false;
            }
        }
        else
        {
            buf[3] = 0x01;
            buf += 4;
            *(uint32_t *) buf = htons(ctx->l_target_host);
            buf += 4;
            *(uint16_t *) buf = htons(ctx->target_port);

            if (send(ctx->buf, ctx->l_target_host + 7) < 0)
            {
                return false;
            }
        }

        /**
         * response
         */
        n = recv(buf, sizeof(ctx->buf));
        if (n <= 0)
        {
            return false;
        }

        uchar version = buf[0];
        if (version != SW_SOCKS5_VERSION_CODE)
        {
            swoole_error_log(SW_LOG_NOTICE, SW_ERROR_SOCKS5_UNSUPPORT_VERSION, "SOCKS version is not supported.");
            return false;
        }
        uchar result = buf[1];
#if 0
        uchar reg = buf[2];
        uchar type = buf[3];
        uint32_t ip = *(uint32_t *) (buf + 4);
        uint16_t port = *(uint16_t *) (buf + 8);
#endif
        if (result == 0)
        {
            ctx->state = SW_SOCKS5_STATE_READY;
        }
        else
        {
            swoole_error_log(SW_LOG_NOTICE, SW_ERROR_SOCKS5_SERVER_ERROR, "Socks5 server error, reason :%s.",
                    swSocks5_strerror(result));
        }
        return result;
    }
}

bool Socket::http_proxy_handshake()
{
#ifdef SW_USE_OPENSSL
    //https proxy
    if (http_proxy->ssl && ssl_handshake() == false)
    {
        return false;
    }
#endif

    //CONNECT
    int n = snprintf(http_proxy->buf, sizeof(http_proxy->buf), "CONNECT %s:%d HTTP/1.1\r\n\r\n",
            http_proxy->target_host, http_proxy->target_port);
    if (send(http_proxy->buf, n) <= 0)
    {
        return false;
    }

    n = recv(http_proxy->buf, sizeof(http_proxy->buf));
    if (n <= 0)
    {
        return false;
    }
    char *buf = http_proxy->buf;
    int len = n;
    int state = 0;
    char *p = buf;
    for (p = buf; p < buf + len; p++)
    {
        if (state == 0)
        {
            if (strncasecmp(p, "HTTP/1.1", 8) == 0 || strncasecmp(p, "HTTP/1.0", 8) == 0)
            {
                state = 1;
                p += 8;
            }
            else
            {
                break;
            }
        }
        else if (state == 1)
        {
            if (isspace(*p))
            {
                continue;
            }
            else
            {
                if (strncasecmp(p, "200", 3) == 0)
                {
                    state = 2;
                    p += 3;
                }
                else
                {
                    break;
                }
            }
        }
        else if (state == 2)
        {
            if (isspace(*p))
            {
                continue;
            }
            else
            {
                if (strncasecmp(p, "Connection established", sizeof("Connection established") - 1) == 0)
                {
                    return true;
                }
                else
                {
                    break;
                }
            }
        }
    }
    return false;
}

static inline int socket_connect(int fd, struct sockaddr *addr, socklen_t len)
{
    int retval;
    while (1)
    {
        retval = ::connect(fd, addr, len);
        if (retval < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
        }
        break;
    }
    return retval;
}

Socket::Socket(enum swSocket_type _type)
{
    type = _type;
    switch (type)
    {
    case SW_SOCK_TCP6:
        _sock_domain = AF_INET6;
        _sock_type = SOCK_STREAM;
        break;
    case SW_SOCK_UNIX_STREAM:
        _sock_domain = AF_UNIX;
        _sock_type = SOCK_STREAM;
        break;
    case SW_SOCK_UDP:
        _sock_domain = AF_INET;
        _sock_type = SOCK_DGRAM;
        break;
    case SW_SOCK_UDP6:
        _sock_domain = AF_INET6;
        _sock_type = SOCK_DGRAM;
        break;
    case SW_SOCK_UNIX_DGRAM:
        _sock_domain = AF_UNIX;
        _sock_type = SOCK_DGRAM;
        break;
    case SW_SOCK_TCP:
    default:
        _sock_domain = AF_INET;
        _sock_type = SOCK_STREAM;
        break;
    }

#ifdef SOCK_CLOEXEC
    int sockfd = ::socket(_sock_domain, _sock_type | SOCK_CLOEXEC, 0);
#else
    int sockfd = ::socket(_sock_domain, _sock_type, 0);
#endif
    if (sockfd < 0)
    {
        swWarn("socket() failed. Error: %s[%d]", strerror(errno), errno);
        return;
    }

    if (swIsMaster() && SwooleTG.type == SW_THREAD_REACTOR)
    {
        reactor = SwooleTG.reactor;
    }
    else
    {
        reactor = SwooleG.main_reactor;
    }
    socket = swReactor_get(reactor, sockfd);

    bzero(socket, sizeof(swConnection));
    socket->fd = sockfd;
    socket->object = this;
    socket->socket_type = type;

    swSetNonBlock(socket->fd);
    if (!swReactor_handle_isset(reactor, SW_FD_CORO_SOCKET))
    {
        reactor->setHandle(reactor, SW_FD_CORO_SOCKET | SW_EVENT_READ, socket_onRead);
        reactor->setHandle(reactor, SW_FD_CORO_SOCKET | SW_EVENT_WRITE, socket_onWrite);
        reactor->setHandle(reactor, SW_FD_CORO_SOCKET | SW_EVENT_ERROR, socket_onRead);
    }
    init();
}

Socket::Socket(int _fd, Socket *sock)
{
    reactor = sock->reactor;

    socket = swReactor_get(reactor, _fd);
    bzero(socket, sizeof(swConnection));
    socket->fd = _fd;
    socket->object = this;
    socket->socket_type = sock->type;

    _sock_domain = sock->_sock_domain;
    _sock_type = sock->_sock_type;
    init();
}

bool Socket::connect(string host, int port, int flags)
{
    //enable socks5 proxy
    if (socks5_proxy)
    {
        socks5_proxy->target_host = (char *) host.c_str();
        socks5_proxy->l_target_host = host.size();
        socks5_proxy->target_port = port;

        host = socks5_proxy->host;
        port = socks5_proxy->port;
    }

    //enable http proxy
    if (http_proxy)
    {
        http_proxy->target_host = (char *) host.c_str();
        http_proxy->target_port = host.size();

        host = http_proxy->proxy_host;
        port = http_proxy->proxy_port;
    }

    if (_sock_domain == AF_INET6 || _sock_domain == AF_INET)
    {
        if (port == -1)
        {
            swWarn("Socket of type AF_INET/AF_INET6 requires port argument");
            return false;
        }
        else if (port == 0 || port >= 65536)
        {
            swWarn("Invalid port argument[%d]", port);
            return false;
        }
    }

    if (unlikely(_cid && _cid != coroutine_get_cid()))
    {
        swWarn( "socket has already been bound to another coroutine.");
        return false;
    }

    int retval = 0;
    _host = host;
    _port = port;

    for (int i = 0; i < 2; i++)
    {
        if (_sock_domain == AF_INET)
        {
            socket->info.addr.inet_v4.sin_family = AF_INET;
            socket->info.addr.inet_v4.sin_port = htons(port);

            if (!inet_pton(AF_INET, _host.c_str(), & socket->info.addr.inet_v4.sin_addr))
            {
                _host = resolve(_host);
                if (_host.size() == 0)
                {
                    return false;
                }
                continue;
            }
            else
            {
                socket->info.len = sizeof( socket->info.addr.inet_v4);
                retval = socket_connect(socket->fd, (struct sockaddr *) &socket->info.addr.inet_v4, socket->info.len);
                break;
            }
        }
        else if (_sock_domain == AF_INET6)
        {
            socket->info.addr.inet_v6.sin6_family = AF_INET6;
            socket->info.addr.inet_v6.sin6_port = htons(port);

            if (!inet_pton(AF_INET6, _host.c_str(), &socket->info.addr.inet_v6.sin6_addr))
            {
                _host = resolve(_host);
                if (_host.size() == 0)
                {
                    return false;
                }
                continue;
            }
            else
            {
                socket->info.len = sizeof(socket->info.addr.inet_v6);
                retval = socket_connect(socket->fd, (struct sockaddr *) &socket->info.addr.inet_v6, socket->info.len);
                break;
            }
        }
        else if (_sock_domain == AF_UNIX)
        {
            if (_host.size() >= sizeof(socket->info.addr.un.sun_path))
            {
                return false;
            }
            socket->info.addr.un.sun_family = AF_UNIX;
            memcpy(&socket->info.addr.un.sun_path, _host.c_str(), _host.size());
            retval = socket_connect(socket->fd, (struct sockaddr *) &socket->info.addr.un,
                    (socklen_t) (offsetof(struct sockaddr_un, sun_path) + _host.size()));
            break;
        }
        else
        {
            return false;
        }
    }

    if (retval == -1)
    {
        if (errno != EINPROGRESS)
        {
            _error: errCode = errno;
            return false;
        }
        if (!wait_events(SW_EVENT_WRITE))
        {
            goto _error;
        }
        yield();
        //Connection has timed out
        if (errCode == ETIMEDOUT)
        {
            errMsg = strerror(errCode);
            return false;
        }
        socklen_t len = sizeof(errCode);
        if (getsockopt(socket->fd, SOL_SOCKET, SO_ERROR, &errCode, &len) < 0 || errCode != 0)
        {
            errMsg = strerror(errCode);
            return false;
        }
    }
    socket->active = 1;
    //socks5 proxy
    if (socks5_proxy && socks5_handshake() == false)
    {
        return false;
    }
    //http proxy
    if (http_proxy && http_proxy_handshake() == false)
    {
        return false;
    }
    return true;
}

static void socket_onResolveCompleted(swAio_event *event)
{
    Socket *sock = (Socket *) event->object;
    if (event->error != 0)
    {
        sock->errCode = SW_ERROR_DNSLOOKUP_RESOLVE_FAILED;
    }
    sock->resume();
}

static void socket_onTimeout(swTimer *timer, swTimer_node *tnode)
{
    Socket *sock = (Socket *) tnode->data;
    sock->timer = NULL;
    sock->errCode = ETIMEDOUT;
    sock->reactor->del(sock->reactor, sock->socket->fd);
    sock->resume();
}

static int socket_onRead(swReactor *reactor, swEvent *event)
{
    Socket *sock = (Socket *) event->socket->object;
    reactor->del(reactor, event->fd);
    sock->resume();
    return SW_OK;
}

static int socket_onWrite(swReactor *reactor, swEvent *event)
{
    Socket *sock = (Socket *) event->socket->object;
    reactor->del(reactor, event->fd);
    sock->resume();
    return SW_OK;
}

ssize_t Socket::peek(void *__buf, size_t __n)
{
    return swConnection_peek(socket, __buf, __n, 0);
}

ssize_t Socket::recv(void *__buf, size_t __n)
{
    ssize_t retval = swConnection_recv(socket, __buf, __n, 0);
    if (retval >= 0 || errno != EAGAIN)
    {
        return retval;
    }

    int events = SW_EVENT_READ;
#ifdef SW_USE_OPENSSL
    if (socket->ssl && socket->ssl_want_write)
    {
        events = SW_EVENT_WRITE;
    }
#endif
    if (!wait_events(events))
    {
        return -1;
    }
    yield();
    if (errCode == ETIMEDOUT)
    {
        return -1;
    }
    retval = swConnection_recv(socket, __buf, __n, 0);
    if (retval < 0)
    {
        errCode = errno;
    }
    return retval;
}

ssize_t Socket::recv_all(void *__buf, size_t __n)
{
    ssize_t retval, total_bytes = 0;
    while (true)
    {
        retval = recv((char*) __buf + total_bytes, __n - total_bytes);
        if (retval <= 0)
        {
            break;
        }
        total_bytes += retval;
        if (total_bytes == __n)
        {
            break;
        }
    }
    return total_bytes;
}

ssize_t Socket::send_all(const void *__buf, size_t __n)
{
    ssize_t retval, total_bytes = 0;
    while (true)
    {
        retval = send((char*) __buf + total_bytes, __n - total_bytes);
        if (retval <= 0)
        {
            break;
        }
        total_bytes += retval;
        if (total_bytes == __n)
        {
            break;
        }
    }
    return total_bytes;
}

ssize_t Socket::send(const void *__buf, size_t __n)
{
    ssize_t n = swConnection_send(socket, (void *) __buf, __n, 0);
    if (n >= 0)
    {
        return n;
    }
    if (errno != EAGAIN)
    {
        return n;
    }
    int events = SW_EVENT_WRITE;
#ifdef SW_USE_OPENSSL
    if (socket->ssl && socket->ssl_want_read)
    {
        events = SW_EVENT_READ;
    }
#endif
    if (!wait_events(events))
    {
        return -1;
    }
    yield();
    if (errCode == ETIMEDOUT)
    {
        return -1;
    }
    ssize_t retval = swConnection_send(socket, (void *) __buf, __n, 0);
    if (retval < 0)
    {
        errCode = errno;
    }
    return retval;
}

void Socket::yield()
{
    if (suspending)
    {
        swError("socket has already been bound to another coroutine.");
    }
    errCode = 0;
    if (_timeout > 0)
    {
        int ms = (int) (_timeout * 1000);
        if (SwooleG.timer.fd == 0)
        {
           swTimer_init(ms);
        }
        timer = SwooleG.timer.add(&SwooleG.timer, ms, 0, this, socket_onTimeout);
    }
    _cid = coroutine_get_cid();
    if (_cid == -1)
    {
        swError("Socket::yield() must be called in the coroutine.");
    }
    //suspend
    suspending = true;
    coroutine_yield(coroutine_get_by_id(_cid));
    suspending = false;
    //clear timer
    if (timer)
    {
        swTimer_del(&SwooleG.timer, timer);
        timer = nullptr;
    }
}

void Socket::resume()
{
    coroutine_resume(coroutine_get_by_id(_cid));
}

bool Socket::bind(std::string address, int port)
{
    bind_address = address;
    bind_port = port;

    struct sockaddr_storage sa_storage = { 0 };
    struct sockaddr *sock_type = (struct sockaddr*) &sa_storage;

    int retval;
    switch (_sock_domain)
    {
    case AF_UNIX:
    {
        struct sockaddr_un *sa = (struct sockaddr_un *) sock_type;
        sa->sun_family = AF_UNIX;

        if (bind_address.size() >= sizeof(sa->sun_path))
        {
            return false;
        }
        memcpy(&sa->sun_path, bind_address.c_str(), bind_address.size());

        retval = ::bind(socket->fd, (struct sockaddr *) sa,
        offsetof(struct sockaddr_un, sun_path) + bind_address.size());
        break;
    }

    case AF_INET:
    {
        struct sockaddr_in *sa = (struct sockaddr_in *) sock_type;
        sa->sin_family = AF_INET;
        sa->sin_port = htons((unsigned short) bind_port);
        if (!inet_aton(bind_address.c_str(), &sa->sin_addr))
        {
            return false;
        }
        retval = ::bind(socket->fd, (struct sockaddr *) sa, sizeof(struct sockaddr_in));
        break;
    }

    case AF_INET6:
    {
        struct sockaddr_in6 *sa = (struct sockaddr_in6 *) sock_type;
        sa->sin6_family = AF_INET6;
        sa->sin6_port = htons((unsigned short) bind_port);

        if (!inet_pton(AF_INET6, bind_address.c_str(), &sa->sin6_addr))
        {
            return false;
        }
        retval = ::bind(socket->fd, (struct sockaddr *) sa, sizeof(struct sockaddr_in6));
        break;
    }
    default:
        return false;
    }

    if (retval != 0)
    {
        errCode = errno;
        return false;
    }

    return true;
}

bool Socket::listen(int backlog)
{
    _backlog = backlog;
    if (::listen(socket->fd, backlog) != 0)
    {
        errCode = errno;
        return false;
    }
    return true;
}

Socket* Socket::accept()
{
    if (!wait_events(SW_EVENT_READ))
    {
        return nullptr;
    }
    yield();
    if (errCode == ETIMEDOUT)
    {
        return nullptr;
    }
    int conn;
    swSocketAddress client_addr;
    socklen_t client_addrlen = sizeof(client_addr);

#ifdef HAVE_ACCEPT4
    conn = ::accept4(socket->fd, (struct sockaddr *) &client_addr, &client_addrlen, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    conn = ::accept(socket->fd, (struct sockaddr *) &client_addr, &client_addrlen);
    if (conn >= 0)
    {
        swoole_fcntl_set_option(conn, 1, 1);
    }
#endif
    if (conn >= 0)
    {
        return new Socket(conn, this);
    }
    else
    {
        errCode = errno;
        return nullptr;
    }
}

string Socket::resolve(string domain_name)
{
    swAio_event ev;
    bzero(&ev, sizeof(swAio_event));
    ev.nbytes = SW_IP_MAX_LENGTH;
    ev.buf = sw_malloc(ev.nbytes);
    if (!ev.buf)
    {
        errCode = errno;
        return "";
    }

    memcpy(ev.buf, domain_name.c_str(), domain_name.size());
    ((char *) ev.buf)[domain_name.size()] = 0;
    ev.flags = _sock_domain;
    ev.type = SW_AIO_GETHOSTBYNAME;
    ev.object = this;
    ev.callback = socket_onResolveCompleted;

    if (SwooleAIO.init == 0)
    {
        swAio_init();
    }

    if (swAio_dispatch(&ev) < 0)
    {
        errCode = SwooleG.error;
        sw_free(ev.buf);
        return "";
    }
    /**
     * cannot timeout
     */
    double tmp_timeout = _timeout;
    _timeout = -1;
    yield();
    _timeout = tmp_timeout;

    if (errCode == SW_ERROR_DNSLOOKUP_RESOLVE_FAILED)
    {
        errMsg = hstrerror(ev.error);
        return "";
    }
    else
    {
        string addr((char *) ev.buf);
        sw_free(ev.buf);
        return addr;
    }
}

bool Socket::shutdown(int __how)
{
    if (!socket || socket->closed)
    {
        return false;
    }
    if (__how == SHUT_RD)
    {
        if (shutdown_read || shutdow_rw || ::shutdown(socket->fd, SHUT_RD))
        {
            return false;
        }
        else
        {
            shutdown_read = 1;
            return true;
        }
    }
    else if (__how == SHUT_WR)
    {
        if (shutdown_write || shutdow_rw || ::shutdown(socket->fd, SHUT_RD) < 0)
        {
            return false;
        }
        else
        {
            shutdown_write = 1;
            return true;
        }
    }
    else if (__how == SHUT_RDWR)
    {
        if (shutdow_rw || ::shutdown(socket->fd, SHUT_RDWR) < 0)
        {
            return false;
        }
        else
        {
            shutdown_read = 1;
            return true;
        }
    }
    else
    {
        return false;
    }
}

bool Socket::close()
{
    if (socket == NULL || socket->closed)
    {
        return false;
    }
    socket->closed = 1;

    int fd = socket->fd;

    if (_sock_type == SW_SOCK_UNIX_DGRAM)
    {
        unlink(socket->info.addr.un.sun_path);
    }

#ifdef SW_USE_OPENSSL
    if (open_ssl && ssl_context)
    {
        if (socket->ssl)
        {
            swSSL_close(socket);
        }
        swSSL_free_context(ssl_context);
        if (ssl_option.cert_file)
        {
            sw_free(ssl_option.cert_file);
        }
        if (ssl_option.key_file)
        {
            sw_free(ssl_option.key_file);
        }
        if (ssl_option.passphrase)
        {
            sw_free(ssl_option.passphrase);
        }
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
        if (ssl_option.tls_host_name)
        {
            sw_free(ssl_option.tls_host_name);
        }
#endif
        if (ssl_option.cafile)
        {
            sw_free(ssl_option.cafile);
        }
        if (ssl_option.capath)
        {
            sw_free(ssl_option.capath);
        }
    }
#endif
    if (_sock_type == SW_SOCK_UNIX_DGRAM)
    {
        unlink(socket->info.addr.un.sun_path);
    }
    if (timer)
    {
        swTimer_del(&SwooleG.timer, timer);
        timer = NULL;
    }
    socket->active = 0;
    ::close(fd);
    return true;
}

#ifdef SW_USE_OPENSSL
bool Socket::ssl_handshake()
{
    if (socket->ssl)
    {
        return false;
    }

    ssl_context = swSSL_get_context(&ssl_option);
    if (ssl_context == NULL)
    {
        return false;
    }

    if (ssl_option.verify_peer)
    {
        if (swSSL_set_capath(&ssl_option, ssl_context) < 0)
        {
            return false;
        }
    }

    socket->ssl_send = 1;
#if defined(SW_USE_HTTP2) && defined(SW_USE_OPENSSL) && OPENSSL_VERSION_NUMBER >= 0x10002000L
    if (http2)
    {
        if (SSL_CTX_set_alpn_protos(ssl_context, (const unsigned char *) "\x02h2", 3) < 0)
        {
            return false;
        }
    }
#endif

    if (swSSL_create(socket, ssl_context, SW_SSL_CLIENT) < 0)
    {
        return false;
    }
#ifdef SSL_CTRL_SET_TLSEXT_HOSTNAME
    if (ssl_option.tls_host_name)
    {
        SSL_set_tlsext_host_name(socket->ssl, ssl_option.tls_host_name);
    }
#endif

    while (true)
    {
        int retval = swSSL_connect(socket);
        if (retval < 0)
        {
            errCode = SwooleG.error;
            return false;
        }
        if (socket->ssl_state == SW_SSL_STATE_WAIT_STREAM)
        {
            int events = socket->ssl_want_write ? SW_EVENT_WRITE : SW_EVENT_READ;
            if (!wait_events(events))
            {
                return false;
            }
            yield();
            if (errCode == ETIMEDOUT)
            {
                return false;
            }
        }
        else if (socket->ssl_state == SW_SSL_STATE_READY)
        {
            return true;
        }
    }

    if (socket->ssl_state == SW_SSL_STATE_READY && ssl_option.verify_peer)
    {
        if (ssl_verify(ssl_option.allow_self_signed) < 0)
        {
            return false;
        }
    }
    return true;
}

int Socket::ssl_verify(bool allow_self_signed)
{
    if (swSSL_verify(socket, allow_self_signed) < 0)
    {
        return SW_ERR;
    }
    if (ssl_option.tls_host_name && swSSL_check_host(socket, ssl_option.tls_host_name) < 0)
    {
        return SW_ERR;
    }
    return SW_OK;
}
#endif

bool Socket::sendfile(char *filename, off_t offset, size_t length)
{
    int file_fd = open(filename, O_RDONLY);
    if (file_fd < 0)
    {
        swSysError("open(%s) failed.", filename);
        return false;
    }

    if (length == 0)
    {
        struct stat file_stat;
        if (::fstat(file_fd, &file_stat) < 0)
        {
            swSysError("fstat(%s) failed.", filename);
            ::close(file_fd);
            return false;
        }
        length = file_stat.st_size;
    }
    else
    {
        length = offset + length;
    }

    int n, sendn;
    while (offset < length)
    {
        sendn = (length - offset > SW_SENDFILE_CHUNK_SIZE) ? SW_SENDFILE_CHUNK_SIZE : length - offset;
#ifdef SW_USE_OPENSSL
        if (socket->ssl)
        {
            n = swSSL_sendfile(socket, file_fd, &offset, sendn);
        }
        else
#endif
        {
            n = ::swoole_sendfile(socket->fd, file_fd, &offset, sendn);
        }
        if (n > 0)
        {
            continue;
        }
        else if (n == 0)
        {
            swWarn("sendfile return zero.");
            return false;
        }
        else if (errno != EAGAIN)
        {
            swSysError("sendfile(%d, %s) failed.", socket->fd, filename);
            _error: errCode = errno;
            ::close(file_fd);
            return false;
        }
        if (!wait_events(SW_EVENT_WRITE))
        {
            goto _error;
        }
        yield();
        if (errCode == ETIMEDOUT)
        {
            goto _error;
        }
    }
    ::close(file_fd);
    return false;
}

int Socket::sendto(char *address, int port, char *data, int len)
{
    if (type == SW_SOCK_UDP)
    {
        return swSocket_udp_sendto(socket->fd, address, port, data, len);
    }
    else if (type == SW_SOCK_UDP6)
    {
        return swSocket_udp_sendto6(socket->fd, address, port, data, len);
    }
    else
    {
        swWarn("only supports SWOOLE_SOCK_UDP or SWOOLE_SOCK_UDP6.");
        return -1;
    }
}

int Socket::recvfrom(void *__buf, size_t __n, char *address, int *port)
{
    socket->info.len = sizeof(socket->info.addr);
    int retval;

    _recv: retval = ::recvfrom(socket->fd, __buf, __n, 0, (struct sockaddr *) &socket->info.addr, &socket->info.len);
    if (retval < 0)
    {
        if (errno == EINTR)
        {
            goto _recv;
        }
        else if (swConnection_error(errno) == SW_WAIT)
        {
            if (!wait_events(SW_EVENT_READ))
            {
                return -1;
            }
            yield();
            if (errCode == ETIMEDOUT)
            {
                return -1;
            }
            retval = ::recvfrom(socket->fd, __buf, __n, 0, (struct sockaddr *) &socket->info.addr, &socket->info.len);
            if (retval < 0)
            {
                errCode = errno;
            }
            else
            {
                strcpy(address, swConnection_get_ip(socket));
                *port = swConnection_get_port(socket);
            }
        }
        else
        {
            errCode = errno;
        }
    }
    return retval;
}

Socket::~Socket()
{
    if (!socket->closed)
    {
        close();
    }
    if (socket->out_buffer)
    {
        swBuffer_free(socket->out_buffer);
        socket->out_buffer = NULL;
    }
    if (socket->in_buffer)
    {
        swBuffer_free(socket->in_buffer);
        socket->in_buffer = NULL;
    }
    if (buffer)
    {
        swString_free(buffer);
    }
    bzero(socket, sizeof(swConnection));
    socket->removed = 1;
}

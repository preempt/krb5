/* -*- mode: c; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/* lib/apputils/net-server.c - Network code for krb5 servers (kdc, kadmind) */
/*
 * Copyright 1990,2000,2007,2008,2009,2010 by the Massachusetts Institute of Technology.
 *
 * Export of this software from the United States of America may
 *   require a specific license from the United States Government.
 *   It is the responsibility of any person or organization contemplating
 *   export to obtain such a license before exporting.
 *
 * WITHIN THAT CONSTRAINT, permission to use, copy, modify, and
 * distribute this software and its documentation for any purpose and
 * without fee is hereby granted, provided that the above copyright
 * notice appear in all copies and that both that copyright notice and
 * this permission notice appear in supporting documentation, and that
 * the name of M.I.T. not be used in advertising or publicity pertaining
 * to distribution of the software without specific, written prior
 * permission.  Furthermore if you modify this software you must label
 * your software as modified software and not distribute it in such a
 * fashion that it might be confused with the original M.I.T. software.
 * M.I.T. makes no representations about the suitability of
 * this software for any purpose.  It is provided "as is" without express
 * or implied warranty.
 */

#include "k5-int.h"
#include "adm_proto.h"
#include <sys/ioctl.h>
#include <syslog.h>

#include <stddef.h>
#include "port-sockets.h"
#include "socket-utils.h"

#include <gssrpc/rpc.h>

#ifdef HAVE_NETINET_IN_H
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#ifdef HAVE_SYS_SOCKIO_H
/* for SIOCGIFCONF, etc. */
#include <sys/sockio.h>
#endif
#include <sys/time.h>
#if HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include <arpa/inet.h>

#ifndef ARPHRD_ETHER /* OpenBSD breaks on multiple inclusions */
#include <net/if.h>
#endif

#ifdef HAVE_SYS_FILIO_H
#include <sys/filio.h>          /* FIONBIO */
#endif

#include "fake-addrinfo.h"
#include "net-server.h"
#include <signal.h>

#include "udppktinfo.h"

/* XXX */
#define KDC5_NONET                               (-1779992062L)

static int tcp_or_rpc_data_counter;
static int max_tcp_or_rpc_data_connections = 45;

static int
ipv6_enabled()
{
    static int result = -1;
    if (result == -1) {
        int s;
        s = socket(AF_INET6, SOCK_STREAM, 0);
        if (s >= 0) {
            result = 1;
            close(s);
        } else
            result = 0;
    }
    return result;
}

static int
setreuseaddr(int sock, int value)
{
    return setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &value, sizeof(value));
}

#if defined(IPV6_V6ONLY)
static int
setv6only(int sock, int value)
{
    return setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY, &value, sizeof(value));
}
#endif

static const char *
paddr(struct sockaddr *sa)
{
    static char buf[100];
    char portbuf[10];
    if (getnameinfo(sa, sa_socklen(sa),
                    buf, sizeof(buf), portbuf, sizeof(portbuf),
                    NI_NUMERICHOST|NI_NUMERICSERV))
        strlcpy(buf, "<unprintable>", sizeof(buf));
    else {
        unsigned int len = sizeof(buf) - strlen(buf);
        char *p = buf + strlen(buf);
        if (len > 2+strlen(portbuf)) {
            *p++ = '.';
            len--;
            strncpy(p, portbuf, len);
        }
    }
    return buf;
}

/* KDC data.  */

enum conn_type {
    CONN_UDP, CONN_TCP_LISTENER, CONN_TCP, CONN_RPC_LISTENER, CONN_RPC
};

/* Per-connection info.  */
struct connection {
    void *handle;
    const char *prog;
    enum conn_type type;

    /* Connection fields (TCP or RPC) */
    struct sockaddr_storage addr_s;
    socklen_t addrlen;
    char addrbuf[56];
    krb5_fulladdr faddr;
    krb5_address kaddr;

    /* Incoming data (TCP) */
    size_t bufsiz;
    size_t offset;
    char *buffer;
    size_t msglen;

    /* Outgoing data (TCP) */
    krb5_data *response;
    unsigned char lenbuf[4];
    sg_buf sgbuf[2];
    sg_buf *sgp;
    int sgnum;

    /* Crude denial-of-service avoidance support (TCP or RPC) */
    time_t start_time;

    /* RPC-specific fields */
    SVCXPRT *transp;
    int rpc_force_close;
};

#define SET(TYPE) struct { TYPE *data; size_t n, max; }

/* Start at the top and work down -- this should allow for deletions
   without disrupting the iteration, since we delete by overwriting
   the element to be removed with the last element.  */
#define FOREACH_ELT(set,idx,vvar)                                       \
    for (idx = set.n-1; idx >= 0 && (vvar = set.data[idx], 1); idx--)

#define GROW_SET(set, incr, tmpptr)                                     \
    ((set.max + incr < set.max                                          \
      || ((set.max + incr) * sizeof(set.data[0]) / sizeof(set.data[0])  \
          != set.max + incr))                                           \
     ? 0                         /* overflow */                         \
     : ((tmpptr = realloc(set.data,                                     \
                          (set.max + incr) * sizeof(set.data[0])))      \
        ? (set.data = tmpptr, set.max += incr, 1)                       \
        : 0))

/* 1 = success, 0 = failure */
#define ADD(set, val, tmpptr)                           \
    ((set.n < set.max || GROW_SET(set, 10, tmpptr))     \
     ? (set.data[set.n++] = val, 1)                     \
     : 0)

#define DEL(set, idx)                           \
    (set.data[idx] = set.data[--set.n], 0)

#define FREE_SET_DATA(set)                                      \
    (free(set.data), set.data = 0, set.max = 0, set.n = 0)

/*
 * N.B.: The Emacs cc-mode indentation code seems to get confused if
 * the macro argument here is one word only.  So use "unsigned short"
 * instead of the "u_short" we were using before.
 */
struct rpc_svc_data {
    u_short port;
    u_long prognum;
    u_long versnum;
    void (*dispatch)();
};
static SET(unsigned short) udp_port_data, tcp_port_data;
static SET(struct rpc_svc_data) rpc_svc_data;
static SET(verto_ev *) events;

verto_ctx *
loop_init(verto_ev_type types)
{
    types |= VERTO_EV_TYPE_IO;
    types |= VERTO_EV_TYPE_SIGNAL;
    types |= VERTO_EV_TYPE_TIMEOUT;
    return verto_default(NULL, types);
}

static void
do_break(verto_ctx *ctx, verto_ev *ev)
{
    krb5_klog_syslog(LOG_DEBUG, _("Got signal to request exit"));
    verto_break(ctx);
}

struct sighup_context {
    void *handle;
    void (*reset)(void *);
};

static void
do_reset(verto_ctx *ctx, verto_ev *ev)
{
    struct sighup_context *sc = (struct sighup_context*) verto_get_private(ev);

    krb5_klog_syslog(LOG_DEBUG, _("Got signal to reset"));
    krb5_klog_reopen(get_context(sc->handle));
    if (sc->reset)
        sc->reset(sc->handle);
}

static void
free_sighup_context(verto_ctx *ctx, verto_ev *ev)
{
    free(verto_get_private(ev));
}

krb5_error_code
loop_setup_signals(verto_ctx *ctx, void *handle, void (*reset)())
{
    struct sighup_context *sc;
    verto_ev *ev;

    if (!verto_add_signal(ctx, VERTO_EV_FLAG_PERSIST, do_break, SIGINT)  ||
        !verto_add_signal(ctx, VERTO_EV_FLAG_PERSIST, do_break, SIGTERM) ||
        !verto_add_signal(ctx, VERTO_EV_FLAG_PERSIST, do_break, SIGQUIT) ||
        !verto_add_signal(ctx, VERTO_EV_FLAG_PERSIST, VERTO_SIG_IGN, SIGPIPE))
        return ENOMEM;

    ev = verto_add_signal(ctx, VERTO_EV_FLAG_PERSIST, do_reset, SIGHUP);
    if (!ev)
        return ENOMEM;

    sc = malloc(sizeof(*sc));
    if (!sc)
        return ENOMEM;

    sc->handle = handle;
    sc->reset = reset;
    verto_set_private(ev, sc, free_sighup_context);
    return 0;
}

krb5_error_code
loop_add_udp_port(int port)
{
    int i;
    void *tmp;
    u_short val;
    u_short s_port = port;

    if (s_port != port)
        return EINVAL;

    FOREACH_ELT (udp_port_data, i, val)
        if (s_port == val)
            return 0;
    if (!ADD(udp_port_data, s_port, tmp))
        return ENOMEM;
    return 0;
}

krb5_error_code
loop_add_tcp_port(int port)
{
    int i;
    void *tmp;
    u_short val;
    u_short s_port = port;

    if (s_port != port)
        return EINVAL;

    FOREACH_ELT (tcp_port_data, i, val)
        if (s_port == val)
            return 0;
    if (!ADD(tcp_port_data, s_port, tmp))
        return ENOMEM;
    return 0;
}

krb5_error_code
loop_add_rpc_service(int port, u_long prognum,
                     u_long versnum, void (*dispatchfn)())
{
    int i;
    void *tmp;
    struct rpc_svc_data svc, val;

    svc.port = port;
    if (svc.port != port)
        return EINVAL;
    svc.prognum = prognum;
    svc.versnum = versnum;
    svc.dispatch = dispatchfn;

    FOREACH_ELT (rpc_svc_data, i, val) {
        if (val.port == port)
            return 0;
    }
    if (!ADD(rpc_svc_data, svc, tmp))
        return ENOMEM;
    return 0;
}

#define USE_AF AF_INET
#define USE_TYPE SOCK_DGRAM
#define USE_PROTO 0
#define SOCKET_ERRNO errno
#include "foreachaddr.h"

struct socksetup {
    verto_ctx *ctx;
    void *handle;
    const char *prog;
    krb5_error_code retval;
};

static void
free_connection(struct connection *conn)
{
    if (!conn)
        return;
    if (conn->response)
        krb5_free_data(get_context(conn->handle), conn->response);
    if (conn->buffer)
        free(conn->buffer);
    if (conn->type == CONN_RPC_LISTENER && conn->transp != NULL)
        svc_destroy(conn->transp);
    free(conn);
}

static void
remove_event_from_set(verto_ev *ev)
{
    verto_ev *tmp;
    int i;

    /* Remove the event from the events. */
    FOREACH_ELT(events, i, tmp)
        if (tmp == ev) {
            DEL(events, i);
            break;
        }
}

static void
free_socket(verto_ctx *ctx, verto_ev *ev)
{
    struct connection *conn = NULL;
    fd_set fds;
    int fd;

    remove_event_from_set(ev);

    fd = verto_get_fd(ev);
    conn = verto_get_private(ev);

    /* Close the file descriptor. */
    krb5_klog_syslog(LOG_INFO, _("closing down fd %d"), fd);
    if (fd >= 0 && (!conn || conn->type != CONN_RPC || conn->rpc_force_close))
        close(fd);

    /* Free the connection struct. */
    if (conn) {
        switch (conn->type) {
        case CONN_RPC:
            if (conn->rpc_force_close) {
                FD_ZERO(&fds);
                FD_SET(fd, &fds);
                svc_getreqset(&fds);
                if (FD_ISSET(fd, &svc_fdset)) {
                    krb5_klog_syslog(LOG_ERR,
                                     _("descriptor %d closed but still "
                                       "in svc_fdset"),
                                     fd);
                }
            }
            /* Fall through. */
        case CONN_TCP:
            tcp_or_rpc_data_counter--;
            break;
        default:
            break;
        }

        free_connection(conn);
    }
}

static verto_ev *
make_event(verto_ctx *ctx, verto_ev_flag flags, verto_callback callback,
           int sock, struct connection *conn, int addevent)
{
    verto_ev *ev;
    void *tmp;

    ev = verto_add_io(ctx, flags, callback, sock);
    if (!ev) {
        com_err(conn->prog, ENOMEM, _("cannot create io event"));
        return NULL;
    }

    if (addevent) {
        if (!ADD(events, ev, tmp)) {
            com_err(conn->prog, ENOMEM, _("cannot save event"));
            verto_del(ev);
            return NULL;
        }
    }

    verto_set_private(ev, conn, free_socket);
    return ev;
}

static verto_ev *
add_fd(struct socksetup *data, int sock, enum conn_type conntype,
       verto_ev_flag flags, verto_callback callback, int addevent)
{
    struct connection *newconn;

#ifndef _WIN32
    if (sock >= FD_SETSIZE) {
        data->retval = EMFILE;  /* XXX */
        com_err(data->prog, 0,
                _("file descriptor number %d too high"), sock);
        return 0;
    }
#endif
    newconn = malloc(sizeof(*newconn));
    if (newconn == NULL) {
        data->retval = ENOMEM;
        com_err(data->prog, ENOMEM,
                _("cannot allocate storage for connection info"));
        return 0;
    }
    memset(newconn, 0, sizeof(*newconn));
    newconn->handle = data->handle;
    newconn->prog = data->prog;
    newconn->type = conntype;

    return make_event(data->ctx, flags, callback, sock, newconn, addevent);
}

static void process_packet(verto_ctx *ctx, verto_ev *ev);
static void accept_tcp_connection(verto_ctx *ctx, verto_ev *ev);
static void process_tcp_connection_read(verto_ctx *ctx, verto_ev *ev);
static void process_tcp_connection_write(verto_ctx *ctx, verto_ev *ev);
static void accept_rpc_connection(verto_ctx *ctx, verto_ev *ev);
static void process_rpc_connection(verto_ctx *ctx, verto_ev *ev);

static verto_ev *
add_udp_fd(struct socksetup *data, int sock)
{
    return add_fd(data, sock, CONN_UDP,
                  VERTO_EV_FLAG_IO_READ |
                  VERTO_EV_FLAG_PERSIST |
                  VERTO_EV_FLAG_REINITIABLE,
                  process_packet, 1);
}

static verto_ev *
add_tcp_listener_fd(struct socksetup *data, int sock)
{
    return add_fd(data, sock, CONN_TCP_LISTENER,
                  VERTO_EV_FLAG_IO_READ |
                  VERTO_EV_FLAG_PERSIST |
                  VERTO_EV_FLAG_REINITIABLE,
                  accept_tcp_connection, 1);
}

static verto_ev *
add_tcp_read_fd(struct socksetup *data, int sock)
{
    return add_fd(data, sock, CONN_TCP,
                  VERTO_EV_FLAG_IO_READ | VERTO_EV_FLAG_PERSIST,
                  process_tcp_connection_read, 1);
}

/*
 * Create a socket and bind it to addr.  Ensure the socket will work with
 * select().  Set the socket cloexec, reuseaddr, and if applicable v6-only.
 * Does not call listen().  Returns -1 on failure after logging an error.
 */
static int
create_server_socket(struct socksetup *data, struct sockaddr *addr, int type)
{
    int sock;

    sock = socket(addr->sa_family, type, 0);
    if (sock == -1) {
        data->retval = errno;
        com_err(data->prog, errno, _("Cannot create TCP server socket on %s"),
                paddr(addr));
        return -1;
    }
    set_cloexec_fd(sock);

#ifndef _WIN32                  /* Windows FD_SETSIZE is a count. */
    if (sock >= FD_SETSIZE) {
        close(sock);
        com_err(data->prog, 0, _("TCP socket fd number %d (for %s) too high"),
                sock, paddr(addr));
        return -1;
    }
#endif

    if (setreuseaddr(sock, 1) < 0) {
        com_err(data->prog, errno,
                _("Cannot enable SO_REUSEADDR on fd %d"), sock);
    }

    if (addr->sa_family == AF_INET6) {
#ifdef IPV6_V6ONLY
        if (setv6only(sock, 1))
            com_err(data->prog, errno,
                    _("setsockopt(%d,IPV6_V6ONLY,1) failed"), sock);
        else
            com_err(data->prog, 0, _("setsockopt(%d,IPV6_V6ONLY,1) worked"),
                    sock);
#else
        krb5_klog_syslog(LOG_INFO, _("no IPV6_V6ONLY socket option support"));
#endif /* IPV6_V6ONLY */
    }

    if (bind(sock, addr, sa_socklen(addr)) == -1) {
        data->retval = errno;
        com_err(data->prog, errno, _("Cannot bind server socket on %s"),
                paddr(addr));
        close(sock);
        return -1;
    }

    return sock;
}

static verto_ev *
add_rpc_listener_fd(struct socksetup *data, struct rpc_svc_data *svc, int sock)
{
    struct connection *conn;
    verto_ev *ev;

    ev = add_fd(data, sock, CONN_RPC_LISTENER,
                VERTO_EV_FLAG_IO_READ |
                VERTO_EV_FLAG_PERSIST |
                VERTO_EV_FLAG_REINITIABLE,
                accept_rpc_connection, 1);
    if (ev == NULL)
        return NULL;

    conn = verto_get_private(ev);
    conn->transp = svctcp_create(sock, 0, 0);
    if (conn->transp == NULL) {
        krb5_klog_syslog(LOG_ERR,
                         _("Cannot create RPC service: %s; continuing"),
                         strerror(errno));
        verto_del(ev);
        return NULL;
    }

    if (!svc_register(conn->transp, svc->prognum, svc->versnum,
                      svc->dispatch, 0)) {
        krb5_klog_syslog(LOG_ERR,
                         _("Cannot register RPC service: %s; continuing"),
                         strerror(errno));
        verto_del(ev);
        return NULL;
    }

    return ev;
}

static verto_ev *
add_rpc_data_fd(struct socksetup *data, int sock)
{
    return add_fd(data, sock, CONN_RPC,
                  VERTO_EV_FLAG_IO_READ | VERTO_EV_FLAG_PERSIST,
                  process_rpc_connection, 1);
}

static const int one = 1;

static int
setnbio(int sock)
{
    return ioctlsocket(sock, FIONBIO, (const void *)&one);
}

static int
setkeepalive(int sock)
{
    return setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
}

static int
setnolinger(int s)
{
    static const struct linger ling = { 0, 0 };
    return setsockopt(s, SOL_SOCKET, SO_LINGER, &ling, sizeof(ling));
}

/* Returns -1 or socket fd.  */
static int
setup_a_tcp_listener(struct socksetup *data, struct sockaddr *addr)
{
    int sock;

    sock = create_server_socket(data, addr, SOCK_STREAM);
    if (sock == -1)
        return -1;
    if (listen(sock, 5) < 0) {
        com_err(data->prog, errno,
                _("Cannot listen on TCP server socket on %s"), paddr(addr));
        close(sock);
        return -1;
    }
    if (setnbio(sock)) {
        com_err(data->prog, errno,
                _("cannot set listening tcp socket on %s non-blocking"),
                paddr(addr));
        close(sock);
        return -1;
    }
    if (setnolinger(sock)) {
        com_err(data->prog, errno,
                _("disabling SO_LINGER on TCP socket on %s"), paddr(addr));
        close(sock);
        return -1;
    }
    return sock;
}

static int
setup_tcp_listener_ports(struct socksetup *data)
{
    struct sockaddr_in sin4;
    struct sockaddr_in6 sin6;
    int i, port;

    memset(&sin4, 0, sizeof(sin4));
    sin4.sin_family = AF_INET;
    sin4.sin_addr.s_addr = INADDR_ANY;

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_addr = in6addr_any;

    FOREACH_ELT (tcp_port_data, i, port) {
        int s4, s6;

        sa_setport((struct sockaddr *)&sin4, port);
        if (!ipv6_enabled()) {
            s4 = setup_a_tcp_listener(data, (struct sockaddr *)&sin4);
            if (s4 < 0)
                return -1;
            s6 = -1;
        } else {
            s4 = s6 = -1;

            sa_setport((struct sockaddr *)&sin6, port);

            s6 = setup_a_tcp_listener(data, (struct sockaddr *)&sin6);
            if (s6 < 0)
                return -1;

            s4 = setup_a_tcp_listener(data, (struct sockaddr *)&sin4);
        }

        /* Sockets are created, prepare to listen on them. */
        if (s4 >= 0) {
            if (add_tcp_listener_fd(data, s4) == NULL)
                close(s4);
            else {
                krb5_klog_syslog(LOG_INFO, _("listening on fd %d: tcp %s"),
                                 s4, paddr((struct sockaddr *)&sin4));
            }
        }
        if (s6 >= 0) {
            if (add_tcp_listener_fd(data, s6) == NULL) {
                close(s6);
                s6 = -1;
            } else {
                krb5_klog_syslog(LOG_INFO, _("listening on fd %d: tcp %s"),
                                 s6, paddr((struct sockaddr *)&sin6));
            }
            if (s4 < 0)
                krb5_klog_syslog(LOG_INFO,
                                 _("assuming IPv6 socket accepts IPv4"));
        }
    }
    return 0;
}

static int
setup_rpc_listener_ports(struct socksetup *data)
{
    struct sockaddr_in sin4;
    struct sockaddr_in6 sin6;
    int i;
    struct rpc_svc_data svc;

    memset(&sin4, 0, sizeof(sin4));
    sin4.sin_family = AF_INET;
    sin4.sin_addr.s_addr = INADDR_ANY;

    memset(&sin6, 0, sizeof(sin6));
    sin6.sin6_family = AF_INET6;
    sin6.sin6_addr = in6addr_any;

    FOREACH_ELT (rpc_svc_data, i, svc) {
        int s4;
        int s6;

        sa_setport((struct sockaddr *)&sin4, svc.port);
        s4 = create_server_socket(data, (struct sockaddr *)&sin4, SOCK_STREAM);
        if (s4 < 0)
            return -1;

        if (add_rpc_listener_fd(data, &svc, s4) == NULL)
            close(s4);
        else
            krb5_klog_syslog(LOG_INFO, _("listening on fd %d: rpc %s"),
                             s4, paddr((struct sockaddr *)&sin4));

        if (ipv6_enabled()) {
            sa_setport((struct sockaddr *)&sin6, svc.port);
            s6 = create_server_socket(data, (struct sockaddr *)&sin6,
                                      SOCK_STREAM);
            if (s6 < 0)
                return -1;

            if (add_rpc_listener_fd(data, &svc, s6) == NULL)
                close(s6);
            else
                krb5_klog_syslog(LOG_INFO, _("listening on fd %d: rpc %s"),
                                 s6, paddr((struct sockaddr *)&sin6));
        }
    }

    return 0;
}

static int
setup_udp_port_1(struct socksetup *data, struct sockaddr *addr);

static void
setup_udp_ports(struct socksetup *data)
{
    struct sockaddr_in sa;
    struct sockaddr_in6 sa6;

    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    setup_udp_port_1(data, (struct sockaddr *)&sa);

    memset(&sa6, 0, sizeof(sa6));
    sa6.sin6_family = AF_INET6;
    setup_udp_port_1(data, (struct sockaddr *)&sa6);

}

static int
setup_udp_port_1(struct socksetup *data, struct sockaddr *addr)
{
    int sock = -1, i, r;
    u_short port;

    FOREACH_ELT (udp_port_data, i, port) {
        sa_setport(addr, port);
        sock = create_server_socket(data, addr, SOCK_DGRAM);
        if (sock == -1)
            return 1;
        setnbio(sock);
        r = set_pktinfo(sock, addr->sa_family);
        if (r) {
            com_err(data->prog, r,
                    _("Cannot request packet info for udp socket address "
                      "%s port %d"), paddr(addr), port);
            krb5_klog_syslog(LOG_INFO, _("System does not support pktinfo yet "
                                         "binding to a wildcard address.  "
                                         "Packets are not guaranteed to "
                                         "return on the received address."));
        }
        krb5_klog_syslog(LOG_INFO, _("listening on fd %d: udp %s%s"), sock,
                         paddr(addr), r ? "" : " (pktinfo)");
        if (add_udp_fd(data, sock) == 0) {
            close(sock);
            return 1;
        }
    }
    return 0;
}

krb5_error_code
loop_setup_network(verto_ctx *ctx, void *handle, const char *prog)
{
    struct socksetup setup_data;
    verto_ev *ev;
    int i;

    /* Close any open connections. */
    FOREACH_ELT(events, i, ev)
        verto_del(ev);
    events.n = 0;

    setup_data.ctx = ctx;
    setup_data.handle = handle;
    setup_data.prog = prog;
    setup_data.retval = 0;
    krb5_klog_syslog(LOG_INFO, _("setting up network..."));

    setup_udp_ports(&setup_data);
    setup_tcp_listener_ports(&setup_data);
    setup_rpc_listener_ports(&setup_data);
    krb5_klog_syslog (LOG_INFO, _("set up %d sockets"), (int) events.n);
    if (events.n == 0) {
        com_err(prog, 0, _("no sockets set up?"));
        exit (1);
    }

    return 0;
}

void
init_addr(krb5_fulladdr *faddr, struct sockaddr *sa)
{
    switch (sa->sa_family) {
    case AF_INET:
        faddr->address->addrtype = ADDRTYPE_INET;
        faddr->address->length = 4;
        faddr->address->contents = (krb5_octet *) &sa2sin(sa)->sin_addr;
        faddr->port = ntohs(sa2sin(sa)->sin_port);
        break;
    case AF_INET6:
        if (IN6_IS_ADDR_V4MAPPED(&sa2sin6(sa)->sin6_addr)) {
            faddr->address->addrtype = ADDRTYPE_INET;
            faddr->address->length = 4;
            faddr->address->contents = 12 + (krb5_octet *) &sa2sin6(sa)->sin6_addr;
        } else {
            faddr->address->addrtype = ADDRTYPE_INET6;
            faddr->address->length = 16;
            faddr->address->contents = (krb5_octet *) &sa2sin6(sa)->sin6_addr;
        }
        faddr->port = ntohs(sa2sin6(sa)->sin6_port);
        break;
    default:
        faddr->address->addrtype = -1;
        faddr->address->length = 0;
        faddr->address->contents = 0;
        faddr->port = 0;
        break;
    }
}

struct udp_dispatch_state {
    void *handle;
    const char *prog;
    int port_fd;
    krb5_address addr;
    krb5_fulladdr faddr;
    socklen_t saddr_len;
    socklen_t daddr_len;
    struct sockaddr_storage saddr;
    struct sockaddr_storage daddr;
    aux_addressing_info auxaddr;
    krb5_data request;
    char pktbuf[MAX_DGRAM_SIZE];
};

static void
process_packet_response(void *arg, krb5_error_code code, krb5_data *response)
{
    struct udp_dispatch_state *state = arg;
    int cc;

    if (code)
        com_err(state->prog ? state->prog : NULL, code,
                _("while dispatching (udp)"));
    if (code || response == NULL)
        goto out;

    cc = send_to_from(state->port_fd, response->data,
                      (socklen_t) response->length, 0,
                      (struct sockaddr *)&state->saddr, state->saddr_len,
                      (struct sockaddr *)&state->daddr, state->daddr_len,
                      &state->auxaddr);
    if (cc == -1) {
        /* Note that the local address (daddr*) has no port number
         * info associated with it. */
        char saddrbuf[NI_MAXHOST], sportbuf[NI_MAXSERV];
        char daddrbuf[NI_MAXHOST];
        int e = errno;

        if (getnameinfo((struct sockaddr *)&state->daddr, state->daddr_len,
                        daddrbuf, sizeof(daddrbuf), 0, 0,
                        NI_NUMERICHOST) != 0) {
            strlcpy(daddrbuf, "?", sizeof(daddrbuf));
        }

        if (getnameinfo((struct sockaddr *)&state->saddr, state->saddr_len,
                        saddrbuf, sizeof(saddrbuf), sportbuf, sizeof(sportbuf),
                        NI_NUMERICHOST|NI_NUMERICSERV) != 0) {
            strlcpy(saddrbuf, "?", sizeof(saddrbuf));
            strlcpy(sportbuf, "?", sizeof(sportbuf));
        }

        com_err(state->prog, e, _("while sending reply to %s/%s from %s"),
                saddrbuf, sportbuf, daddrbuf);
        goto out;
    }
    if ((size_t)cc != response->length) {
        com_err(state->prog, 0, _("short reply write %d vs %d\n"),
                response->length, cc);
    }

out:
    krb5_free_data(get_context(state->handle), response);
    free(state);
}

static void
process_packet(verto_ctx *ctx, verto_ev *ev)
{
    int cc;
    struct connection *conn;
    struct udp_dispatch_state *state;

    conn = verto_get_private(ev);

    state = malloc(sizeof(*state));
    if (!state) {
        com_err(conn->prog, ENOMEM, _("while dispatching (udp)"));
        return;
    }

    state->handle = conn->handle;
    state->prog = conn->prog;
    state->port_fd = verto_get_fd(ev);
    assert(state->port_fd >= 0);

    state->saddr_len = sizeof(state->saddr);
    state->daddr_len = sizeof(state->daddr);
    memset(&state->auxaddr, 0, sizeof(state->auxaddr));
    cc = recv_from_to(state->port_fd, state->pktbuf, sizeof(state->pktbuf), 0,
                      (struct sockaddr *)&state->saddr, &state->saddr_len,
                      (struct sockaddr *)&state->daddr, &state->daddr_len,
                      &state->auxaddr);
    if (cc == -1) {
        if (errno != EINTR && errno != EAGAIN
            /*
             * This is how Linux indicates that a previous transmission was
             * refused, e.g., if the client timed out before getting the
             * response packet.
             */
            && errno != ECONNREFUSED
        )
            com_err(conn->prog, errno, _("while receiving from network"));
        free(state);
        return;
    }
    if (!cc) { /* zero-length packet? */
        free(state);
        return;
    }

#if 0
    if (state->daddr_len > 0) {
        char addrbuf[100];
        if (getnameinfo(ss2sa(&state->daddr), state->daddr_len,
                        addrbuf, sizeof(addrbuf),
                        0, 0, NI_NUMERICHOST))
            strlcpy(addrbuf, "?", sizeof(addrbuf));
        com_err(conn->prog, 0, _("pktinfo says local addr is %s"), addrbuf);
    }
#endif

    if (state->daddr_len == 0 && conn->type == CONN_UDP) {
        /*
         * An address couldn't be obtained, so the PKTINFO option probably
         * isn't available.  If the socket is bound to a specific address, then
         * try to get the address here.
         */
        state->daddr_len = sizeof(state->daddr);
        if (getsockname(state->port_fd, (struct sockaddr *)&state->daddr,
                        &state->daddr_len) != 0)
            state->daddr_len = 0;
        /* On failure, keep going anyways. */
    }

    state->request.length = cc;
    state->request.data = state->pktbuf;
    state->faddr.address = &state->addr;
    init_addr(&state->faddr, ss2sa(&state->saddr));
    /* This address is in net order. */
    dispatch(state->handle, ss2sa(&state->daddr), &state->faddr,
             &state->request, 0, ctx, process_packet_response, state);
}

static int
kill_lru_tcp_or_rpc_connection(void *handle, verto_ev *newev)
{
    struct connection *c = NULL, *oldest_c = NULL;
    verto_ev *ev, *oldest_ev = NULL;
    int i, fd = -1;

    krb5_klog_syslog(LOG_INFO, _("too many connections"));

    FOREACH_ELT (events, i, ev) {
        if (ev == newev)
            continue;

        c = verto_get_private(ev);
        if (!c)
            continue;
        if (c->type != CONN_TCP && c->type != CONN_RPC)
            continue;
#if 0
        krb5_klog_syslog(LOG_INFO, "fd %d started at %ld",
                         verto_get_fd(oldest_ev),
                         c->start_time);
#endif
        if (oldest_c == NULL
            || oldest_c->start_time > c->start_time) {
            oldest_ev = ev;
            oldest_c = c;
        }
    }
    if (oldest_c != NULL) {
        krb5_klog_syslog(LOG_INFO, _("dropping %s fd %d from %s"),
                         c->type == CONN_RPC ? "rpc" : "tcp",
                         verto_get_fd(oldest_ev), oldest_c->addrbuf);
        if (oldest_c->type == CONN_RPC)
            oldest_c->rpc_force_close = 1;
        verto_del(oldest_ev);
    }
    return fd;
}

static void
accept_tcp_connection(verto_ctx *ctx, verto_ev *ev)
{
    int s;
    struct sockaddr_storage addr_s;
    struct sockaddr *addr = (struct sockaddr *)&addr_s;
    socklen_t addrlen = sizeof(addr_s);
    struct socksetup sockdata;
    struct connection *newconn, *conn;
    char tmpbuf[10];
    verto_ev *newev;

    conn = verto_get_private(ev);
    s = accept(verto_get_fd(ev), addr, &addrlen);
    if (s < 0)
        return;
    set_cloexec_fd(s);
#ifndef _WIN32
    if (s >= FD_SETSIZE) {
        close(s);
        return;
    }
#endif
    setnbio(s), setnolinger(s), setkeepalive(s);

    sockdata.ctx = ctx;
    sockdata.handle = conn->handle;
    sockdata.prog = conn->prog;
    sockdata.retval = 0;

    newev = add_tcp_read_fd(&sockdata, s);
    if (newev == NULL) {
        close(s);
        return;
    }
    newconn = verto_get_private(newev);

    if (getnameinfo((struct sockaddr *)&addr_s, addrlen,
                    newconn->addrbuf, sizeof(newconn->addrbuf),
                    tmpbuf, sizeof(tmpbuf),
                    NI_NUMERICHOST | NI_NUMERICSERV))
        strlcpy(newconn->addrbuf, "???", sizeof(newconn->addrbuf));
    else {
        char *p, *end;
        p = newconn->addrbuf;
        end = p + sizeof(newconn->addrbuf);
        p += strlen(p);
        if ((size_t)(end - p) > 2 + strlen(tmpbuf)) {
            *p++ = '.';
            strlcpy(p, tmpbuf, end - p);
        }
    }
#if 0
    krb5_klog_syslog(LOG_INFO, "accepted TCP connection on socket %d from %s",
                     s, newconn->addrbuf);
#endif

    newconn->addr_s = addr_s;
    newconn->addrlen = addrlen;
    newconn->bufsiz = 1024 * 1024;
    newconn->buffer = malloc(newconn->bufsiz);
    newconn->start_time = time(0);

    if (++tcp_or_rpc_data_counter > max_tcp_or_rpc_data_connections)
        kill_lru_tcp_or_rpc_connection(conn->handle, newev);

    if (newconn->buffer == 0) {
        com_err(conn->prog, errno,
                _("allocating buffer for new TCP session from %s"),
                newconn->addrbuf);
        verto_del(newev);
        return;
    }
    newconn->offset = 0;
    newconn->faddr.address = &newconn->kaddr;
    init_addr(&newconn->faddr, ss2sa(&newconn->addr_s));
    SG_SET(&newconn->sgbuf[0], newconn->lenbuf, 4);
    SG_SET(&newconn->sgbuf[1], 0, 0);
}

struct tcp_dispatch_state {
    struct sockaddr_storage local_saddr;
    struct connection *conn;
    krb5_data request;
    verto_ctx *ctx;
    int sock;
};

static void
process_tcp_response(void *arg, krb5_error_code code, krb5_data *response)
{
    struct tcp_dispatch_state *state = arg;
    verto_ev *ev;

    assert(state);
    state->conn->response = response;

    if (code)
        com_err(state->conn->prog, code, _("while dispatching (tcp)"));
    if (code || !response)
        goto kill_tcp_connection;

    /* Queue outgoing response. */
    store_32_be(response->length, state->conn->lenbuf);
    SG_SET(&state->conn->sgbuf[1], response->data, response->length);
    state->conn->sgp = state->conn->sgbuf;
    state->conn->sgnum = 2;

    ev = make_event(state->ctx, VERTO_EV_FLAG_IO_WRITE | VERTO_EV_FLAG_PERSIST,
                    process_tcp_connection_write, state->sock, state->conn, 1);
    if (ev) {
        free(state);
        return;
    }

kill_tcp_connection:
    tcp_or_rpc_data_counter--;
    free_connection(state->conn);
    close(state->sock);
    free(state);
}

/* Creates the tcp_dispatch_state and deletes the verto event. */
static struct tcp_dispatch_state *
prepare_for_dispatch(verto_ctx *ctx, verto_ev *ev)
{
    struct tcp_dispatch_state *state;

    state = malloc(sizeof(*state));
    if (!state) {
        krb5_klog_syslog(LOG_ERR, _("error allocating tcp dispatch private!"));
        return NULL;
    }
    state->conn = verto_get_private(ev);
    state->sock = verto_get_fd(ev);
    state->ctx = ctx;
    verto_set_private(ev, NULL, NULL); /* Don't close the fd or free conn! */
    remove_event_from_set(ev); /* Remove it from the set. */
    verto_del(ev);
    return state;
}

static void
process_tcp_connection_read(verto_ctx *ctx, verto_ev *ev)
{
    struct tcp_dispatch_state *state = NULL;
    struct connection *conn = NULL;
    ssize_t nread;
    size_t len;

    conn = verto_get_private(ev);

    /*
     * Read message length and data into one big buffer, already allocated
     * at connect time.  If we have a complete message, we stop reading, so
     * we should only be here if there is no data in the buffer, or only an
     * incomplete message.
     */
    if (conn->offset < 4) {
        krb5_data *response = NULL;

        /* msglen has not been computed.  XXX Doing at least two reads
         * here, letting the kernel worry about buffering. */
        len = 4 - conn->offset;
        nread = SOCKET_READ(verto_get_fd(ev),
                            conn->buffer + conn->offset, len);
        if (nread < 0) /* error */
            goto kill_tcp_connection;
        if (nread == 0) /* eof */
            goto kill_tcp_connection;
        conn->offset += nread;
        if (conn->offset == 4) {
            unsigned char *p = (unsigned char *)conn->buffer;
            conn->msglen = load_32_be(p);
            if (conn->msglen > conn->bufsiz - 4) {
                krb5_error_code err;
                /* Message too big. */
                krb5_klog_syslog(LOG_ERR, _("TCP client %s wants %lu bytes, "
                                            "cap is %lu"), conn->addrbuf,
                                 (unsigned long) conn->msglen,
                                 (unsigned long) conn->bufsiz - 4);
                /* XXX Should return an error.  */
                err = make_toolong_error (conn->handle,
                                          &response);
                if (err) {
                    krb5_klog_syslog(LOG_ERR, _("error constructing "
                                                "KRB_ERR_FIELD_TOOLONG error! %s"),
                                     error_message(err));
                    goto kill_tcp_connection;
                }

                state = prepare_for_dispatch(ctx, ev);
                if (!state) {
                    krb5_free_data(get_context(conn->handle), response);
                    goto kill_tcp_connection;
                }
                process_tcp_response(state, 0, response);
            }
        }
    } else {
        /* msglen known. */
        socklen_t local_saddrlen = sizeof(struct sockaddr_storage);
        struct sockaddr *local_saddrp = NULL;

        len = conn->msglen - (conn->offset - 4);
        nread = SOCKET_READ(verto_get_fd(ev),
                            conn->buffer + conn->offset, len);
        if (nread < 0) /* error */
            goto kill_tcp_connection;
        if (nread == 0) /* eof */
            goto kill_tcp_connection;
        conn->offset += nread;
        if (conn->offset < conn->msglen + 4)
            return;

        /* Have a complete message, and exactly one message. */
        state = prepare_for_dispatch(ctx, ev);
        if (!state)
            goto kill_tcp_connection;

        state->request.length = conn->msglen;
        state->request.data = conn->buffer + 4;

        if (getsockname(verto_get_fd(ev), ss2sa(&state->local_saddr),
                        &local_saddrlen) == 0)
            local_saddrp = ss2sa(&state->local_saddr);

        dispatch(state->conn->handle, local_saddrp, &conn->faddr,
                 &state->request, 1, ctx, process_tcp_response, state);
    }

    return;

kill_tcp_connection:
    verto_del(ev);
}

static void
process_tcp_connection_write(verto_ctx *ctx, verto_ev *ev)
{
    struct connection *conn;
    SOCKET_WRITEV_TEMP tmp;
    ssize_t nwrote;
    int sock;

    conn = verto_get_private(ev);
    sock = verto_get_fd(ev);

    nwrote = SOCKET_WRITEV(sock, conn->sgp,
                           conn->sgnum, tmp);
    if (nwrote > 0) { /* non-error and non-eof */
        while (nwrote) {
            sg_buf *sgp = conn->sgp;
            if ((size_t)nwrote < SG_LEN(sgp)) {
                SG_ADVANCE(sgp, (size_t)nwrote);
                nwrote = 0;
            } else {
                nwrote -= SG_LEN(sgp);
                conn->sgp++;
                conn->sgnum--;
                if (conn->sgnum == 0 && nwrote != 0)
                    abort();
            }
        }

        /* If we still have more data to send, just return so that
         * the main loop can call this function again when the socket
         * is ready for more writing. */
        if (conn->sgnum > 0)
            return;
    }

    /* Finished sending.  We should go back to reading, though if we
     * sent a FIELD_TOOLONG error in reply to a length with the high
     * bit set, RFC 4120 says we have to close the TCP stream. */
    verto_del(ev);
}

void
loop_free(verto_ctx *ctx)
{
    verto_free(ctx);
    FREE_SET_DATA(events);
    FREE_SET_DATA(udp_port_data);
    FREE_SET_DATA(tcp_port_data);
    FREE_SET_DATA(rpc_svc_data);
}

static int
have_event_for_fd(int fd)
{
    verto_ev *ev;
    int i;

    FOREACH_ELT(events, i, ev) {
        if (verto_get_fd(ev) == fd)
            return 1;
    }

    return 0;
}

static void
accept_rpc_connection(verto_ctx *ctx, verto_ev *ev)
{
    struct socksetup sockdata;
    struct connection *conn;
    fd_set fds;
    register int s;

    conn = verto_get_private(ev);

    sockdata.ctx = ctx;
    sockdata.handle = conn->handle;
    sockdata.prog = conn->prog;
    sockdata.retval = 0;

    /* Service the woken RPC listener descriptor. */
    FD_ZERO(&fds);
    FD_SET(verto_get_fd(ev), &fds);
    svc_getreqset(&fds);

    /* Scan svc_fdset for any new connections. */
    for (s = 0; s < FD_SETSIZE; s++) {
        struct sockaddr_storage addr_s;
        struct sockaddr *addr = (struct sockaddr *) &addr_s;
        socklen_t addrlen = sizeof(addr_s);
        struct connection *newconn;
        char tmpbuf[10];
        verto_ev *newev;

        /* If we already have this fd, continue. */
        if (!FD_ISSET(s, &svc_fdset) || have_event_for_fd(s))
            continue;

        newev = add_rpc_data_fd(&sockdata, s);
        if (newev == NULL)
            continue;
        newconn = verto_get_private(newev);

        set_cloexec_fd(s);
#if 0
        setnbio(s), setnolinger(s), setkeepalive(s);
#endif

        if (getpeername(s, addr, &addrlen) ||
            getnameinfo(addr, addrlen,
                        newconn->addrbuf,
                        sizeof(newconn->addrbuf),
                        tmpbuf, sizeof(tmpbuf),
                        NI_NUMERICHOST | NI_NUMERICSERV)) {
            strlcpy(newconn->addrbuf, "???",
                    sizeof(newconn->addrbuf));
        } else {
            char *p, *end;
            p = newconn->addrbuf;
            end = p + sizeof(newconn->addrbuf);
            p += strlen(p);
            if ((size_t)(end - p) > 2 + strlen(tmpbuf)) {
                *p++ = '.';
                strlcpy(p, tmpbuf, end - p);
            }
        }
#if 0
        krb5_klog_syslog(LOG_INFO, _("accepted RPC connection on socket %d "
                                     "from %s"), s, newconn->addrbuf);
#endif

        newconn->addr_s = addr_s;
        newconn->addrlen = addrlen;
        newconn->start_time = time(0);

        if (++tcp_or_rpc_data_counter > max_tcp_or_rpc_data_connections)
            kill_lru_tcp_or_rpc_connection(newconn->handle, newev);

        newconn->faddr.address = &newconn->kaddr;
        init_addr(&newconn->faddr, ss2sa(&newconn->addr_s));
    }
}

static void
process_rpc_connection(verto_ctx *ctx, verto_ev *ev)
{
    fd_set fds;

    FD_ZERO(&fds);
    FD_SET(verto_get_fd(ev), &fds);
    svc_getreqset(&fds);

    if (!FD_ISSET(verto_get_fd(ev), &svc_fdset))
        verto_del(ev);
}

#endif /* INET */

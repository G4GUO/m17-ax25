/*
 * m17bridge - KISS server with packet mode and loopback for local testing.
 *
 * Listens for KISS clients (BPQ, Direwolf, a hand-written client) and either
 * loops each frame back to every client or, with loopback off, reports what it
 * would have transmitted.  No radio is involved, so the whole chain - KISS
 * decode, packet framing, reassembly, KISS encode - can be exercised on a
 * desktop.
 *
 * Frames are delivered to every connected client, as a shared simplex channel
 * would be.  A single client therefore sees its own frames echoed back, which
 * is what makes one-instance testing possible.
 *
 * Commands arrive as plain text lines, one per line, either on the control
 * port or on stdin:
 *
 *     enable             accept KISS frames and transmit them
 *     disable            drop KISS frames (counted, not reported to the host)
 *     loopback on|off    echo (default) or discard what would be transmitted
 *     status             one line of state and counters
 *     quit               shut down
 *     help               list the commands
 *
 * BSD Zero Clause License.  See LICENSE.
 */
#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "kiss.h"
#include "m17_bridge.h"
#include "m17_packet.h"

#define MAX_CLIENTS 4
#define CLIENT_BUF  1024
#define LINE_BUF    256

typedef struct {
    int          fd;
    int          used;
    kiss_decoder dec;
    uint8_t      buf[CLIENT_BUF];
} kiss_client;

static kiss_client g_kiss[MAX_CLIENTS];
static int         g_ctl[MAX_CLIENTS];
static char        g_ctl_line[MAX_CLIENTS][LINE_BUF];
static size_t      g_ctl_used[MAX_CLIENTS];

static int         g_kiss_listen = -1;
static int         g_ctl_listen  = -1;
static int         g_quit;
static int         g_enabled;
static int         g_loopback = 1;
static int         g_stdin_open = 1;

static m17_bridge  g_bridge;
static uint32_t    g_rng = 0x12345678u;

static unsigned    g_rx_frames;
static unsigned    g_rejected;
static unsigned    g_delivered;
static unsigned    g_no_radio;

static uint32_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + (uint32_t)ts.tv_nsec / 1000000u);
}

static uint32_t next_rand(void)
{
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

static void logmsg(const char *fmt, ...)
{
    va_list ap;

    printf("%8lu ", (unsigned long)now_ms());
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}

static void set_nonblock(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags >= 0)
        (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int listen_on(unsigned port, unsigned *actual)
{
    struct sockaddr_in sa;
    socklen_t          slen = sizeof sa;
    int                fd;
    int                one = 1;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);

    memset(&sa, 0, sizeof sa);
    sa.sin_family      = AF_INET;
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    sa.sin_port        = htons((uint16_t)port);

    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) < 0 || listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    if (getsockname(fd, (struct sockaddr *)&sa, &slen) == 0 && actual != NULL)
        *actual = ntohs(sa.sin_port);

    set_nonblock(fd);
    return fd;
}

/* ------------------------------------------------------------------ output */

static void write_all(int fd, const uint8_t *data, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = send(fd, data + off, len - off, MSG_NOSIGNAL);

        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return; /* a slow client: drop the frame rather than stall the loop */
        } else {
            return;
        }
    }
}

static int client_count(void)
{
    int n = 0;

    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_kiss[i].used)
            n++;
    return n;
}

static void deliver(const uint8_t *frame, size_t len)
{
    uint8_t wire[CLIENT_BUF];
    size_t  n = kiss_encode(0, 0, frame, len, wire, sizeof wire);

    if (n == 0) {
        logmsg("frame too long to re-encode (%lu bytes)", (unsigned long)len);
        return;
    }

    g_delivered++;
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (g_kiss[i].used)
            write_all(g_kiss[i].fd, wire, n);
}

/* The loopback "radio": frame one packet and immediately reassemble it. */
static void loopback_send(const uint8_t *frame, size_t len)
{
    m17_pkt_burst    burst;
    m17_rx_assembler rx;
    m17_pkt_status   st = M17_PKT_NEED_MORE;

    size_t frames = m17_pkt_build(&burst, M17_TYPE_RAW, frame, len);
    if (frames == 0) {
        g_rejected++;
        logmsg("reject %lu byte frame (does not fit an M17 packet)",
               (unsigned long)len);
        return;
    }

    m17_rx_reset(&rx);
    for (size_t i = 0; i < frames; i++) {
        st = m17_rx_chunk(&rx, &burst.frame[i]);
        if (st != M17_PKT_NEED_MORE)
            break;
    }

    if (st == M17_PKT_COMPLETE) {
        /* rx.data[0] is the M17 type specifier; KISS carries the AX.25 frame
         * without it, exactly as it arrived */
        logmsg("loopback %lu -> %lu bytes in %lu frame(s)",
               (unsigned long)len, (unsigned long)rx.len, (unsigned long)frames);
        deliver(&rx.data[1], rx.len - 1);
    } else {
        g_rejected++;
        logmsg("loopback reassembly failed");
    }
}

/* --------------------------------------------------------------- KISS side */

static void on_kiss_frame(void *ctx, const kiss_frame *frame, const uint8_t *data)
{
    (void)ctx;

    if (frame->port != 0 || frame->command != 0)
        return; /* parameter commands (TXDELAY, PERSIST, ...) are not data */

    g_rx_frames++;

    if (frame->overrun) {
        g_rejected++;
        logmsg("drop %lu byte frame: too long for the buffer",
               (unsigned long)frame->len);
        return;
    }
    if (!g_enabled) {
        g_rejected++;
        logmsg("drop %lu byte frame: packet mode disabled",
               (unsigned long)frame->len);
        return;
    }
    if (frame->len == 0)
        return;

    if (!m17_bridge_push(&g_bridge, data, frame->len)) {
        logmsg("queue refused %lu byte frame (dup=%u full=%u)",
               (unsigned long)frame->len, g_bridge.dropped_dup,
               g_bridge.dropped_full);
        return;
    }
    logmsg("queued %lu bytes (depth %u)", (unsigned long)frame->len, g_bridge.count);
}

static void accept_kiss(void)
{
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_kiss[i].used)
            continue;

        int fd = accept(g_kiss_listen, NULL, NULL);
        if (fd < 0)
            return;

        set_nonblock(fd);
        g_kiss[i].fd   = fd;
        g_kiss[i].used = 1;
        kiss_decoder_init(&g_kiss[i].dec, g_kiss[i].buf, sizeof g_kiss[i].buf);
        logmsg("kiss client %d connected", i);
        return;
    }
    logmsg("kiss connection refused: %d clients already", MAX_CLIENTS);
}

static void read_kiss(int idx)
{
    uint8_t chunk[256];
    ssize_t n = recv(g_kiss[idx].fd, chunk, sizeof chunk, 0);

    if (n > 0) {
        kiss_decoder_push(&g_kiss[idx].dec, chunk, (size_t)n, on_kiss_frame,
                          &g_kiss[idx]);
        return;
    }
    if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
        logmsg("kiss client %d disconnected", idx);
        close(g_kiss[idx].fd);
        g_kiss[idx].used = 0;
        g_kiss[idx].fd   = -1;
    }
}

/* ----------------------------------------------------------- control side */

static void control_reply(int fd, const char *text)
{
    write_all(fd, (const uint8_t *)text, strlen(text));
}

static void control_status(int fd)
{
    char line[360];

    snprintf(line, sizeof line,
             "status enabled=%d loopback=%d clients=%d queued=%u sent=%u "
             "duplicate=%u full=%u rx=%u delivered=%u rejected=%u noradio=%u\n",
             g_enabled, g_loopback, client_count(), g_bridge.count,
             g_bridge.sent, g_bridge.dropped_dup, g_bridge.dropped_full,
             g_rx_frames, g_delivered, g_rejected, g_no_radio);
    control_reply(fd, line);
}

static void control_exec(int fd, char *line)
{
    char *arg = strchr(line, ' ');

    if (arg != NULL) {
        *arg++ = '\0';
        while (*arg == ' ')
            arg++;
    }

    if (strcmp(line, "enable") == 0) {
        g_enabled = 1;
        control_reply(fd, "ok enabled\n");
    } else if (strcmp(line, "disable") == 0) {
        g_enabled = 0;
        control_reply(fd, "ok disabled\n");
    } else if (strcmp(line, "loopback") == 0) {
        if (arg != NULL && strcmp(arg, "on") == 0) {
            g_loopback = 1;
            control_reply(fd, "ok loopback=on\n");
        } else if (arg != NULL && strcmp(arg, "off") == 0) {
            g_loopback = 0;
            control_reply(fd, "ok loopback=off\n");
        } else {
            control_reply(fd, "error usage: loopback on|off\n");
        }
    } else if (strcmp(line, "status") == 0) {
        control_status(fd);
    } else if (strcmp(line, "quit") == 0) {
        control_reply(fd, "ok bye\n");
        g_quit = 1;
    } else if (strcmp(line, "help") == 0) {
        control_reply(fd,
                      "commands: enable, disable, loopback on|off, status, quit\n");
    } else if (line[0] == '\0') {
        /* ignore blank lines */
    } else {
        control_reply(fd, "error unknown command\n");
    }
}

/* Feed complete lines from buf/used to control_exec, keeping any partial line */
static void run_lines(int fd, char *buf, size_t *used)
{
    char  *start = buf;
    char  *nl;
    size_t left;

    buf[*used] = '\0';

    while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (nl > start && nl[-1] == '\r')
            nl[-1] = '\0';
        control_exec(fd, start);
        start = nl + 1;
    }

    left = strlen(start);
    memmove(buf, start, left);
    *used = left;
}

static void accept_control(void)
{
    int fd = accept(g_ctl_listen, NULL, NULL);

    if (fd < 0)
        return;

    set_nonblock(fd);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_ctl[i] < 0) {
            g_ctl[i]      = fd;
            g_ctl_used[i] = 0;
            g_ctl_line[i][0] = '\0';
            logmsg("control client %d connected", i);
            return;
        }
    }
    close(fd);
}

static void read_control(int idx)
{
    ssize_t n = recv(g_ctl[idx], g_ctl_line[idx] + g_ctl_used[idx],
                     LINE_BUF - g_ctl_used[idx] - 1, 0);

    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
            close(g_ctl[idx]);
            g_ctl[idx]      = -1;
            g_ctl_used[idx] = 0;
            logmsg("control client %d disconnected", idx);
        }
        return;
    }

    g_ctl_used[idx] += (size_t)n;
    run_lines(g_ctl[idx], g_ctl_line[idx], &g_ctl_used[idx]);
}

static char   g_stdin_line[LINE_BUF];
static size_t g_stdin_used;

static void read_stdin(void)
{
    ssize_t n = read(STDIN_FILENO, g_stdin_line + g_stdin_used,
                     LINE_BUF - g_stdin_used - 1);

    if (n <= 0) {
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
            g_stdin_open = 0;
        return;
    }

    g_stdin_used += (size_t)n;
    run_lines(STDOUT_FILENO, g_stdin_line, &g_stdin_used);
}

/* -------------------------------------------------------------------- main */

static void pump_tx(void)
{
    uint32_t now = now_ms();
    size_t   len = 0;

    if (!m17_bridge_tick(&g_bridge, now, 0 /* no radio, so never busy */,
                         0 /* no microphone */, next_rand()))
        return;

    const uint8_t *frame = m17_bridge_head(&g_bridge, &len);
    if (frame == NULL) {
        m17_bridge_sent(&g_bridge, now);
        return;
    }

    if (g_loopback) {
        loopback_send(frame, len);
    } else {
        g_no_radio++;
        logmsg("would transmit %lu bytes (loopback off, no radio attached)",
               (unsigned long)len);
    }

    m17_bridge_sent(&g_bridge, now_ms());
}

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

int main(int argc, char **argv)
{
    unsigned kiss_port = 8001;
    unsigned ctl_port  = 8002;
    unsigned depth     = 0;
    m17_bridge_config cfg;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;

        if (strcmp(a, "--kiss-port") == 0 && v != NULL) {
            kiss_port = (unsigned)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--control-port") == 0 && v != NULL) {
            ctl_port = (unsigned)strtoul(v, NULL, 10);
            i++;
        } else if (strcmp(a, "--enable") == 0 && v != NULL) {
            g_enabled = atoi(v);
            i++;
        } else if (strcmp(a, "--loopback") == 0 && v != NULL) {
            g_loopback = atoi(v);
            i++;
        } else if (strcmp(a, "--queue") == 0 && v != NULL) {
            long q = strtol(v, NULL, 10);
            if (q < 1 || q > M17_BRIDGE_QUEUE_MAX) {
                fprintf(stderr, "queue depth must be 1..%d\n", M17_BRIDGE_QUEUE_MAX);
                return 2;
            }
            depth = (unsigned)q;
            i++;
        } else {
            fprintf(stderr,
                    "usage: %s [--kiss-port N] [--control-port N] "
                    "[--enable 0|1] [--loopback 0|1] [--queue N]\n", argv[0]);
            return 2;
        }
    }

    m17_bridge_config_default(&cfg);
    if (depth != 0)
        cfg.queue_depth = depth;
    m17_bridge_init(&g_bridge, &cfg);

    for (int i = 0; i < MAX_CLIENTS; i++) {
        g_kiss[i].fd = -1;
        g_ctl[i]     = -1;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    g_kiss_listen = listen_on(kiss_port, &kiss_port);
    g_ctl_listen  = listen_on(ctl_port, &ctl_port);

    if (g_kiss_listen < 0 || g_ctl_listen < 0) {
        fprintf(stderr, "cannot listen: %s\n", strerror(errno));
        return 1;
    }

    set_nonblock(STDIN_FILENO);

    logmsg("kiss_port=%u control_port=%u enabled=%d loopback=%d",
           kiss_port, ctl_port, g_enabled, g_loopback);

    while (!g_quit) {
        struct pollfd pfd[3 + MAX_CLIENTS * 2];
        int           map[3 + MAX_CLIENTS * 2];
        int           n = 0;
        int           rc;

        pfd[n].fd = g_kiss_listen;
        pfd[n].events = POLLIN;
        map[n++] = -1;
        pfd[n].fd = g_ctl_listen;
        pfd[n].events = POLLIN;
        map[n++] = -2;
        if (g_stdin_open) {
            pfd[n].fd = STDIN_FILENO;
            pfd[n].events = POLLIN;
            map[n++] = -3;
        }

        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (g_kiss[i].used) {
                pfd[n].fd = g_kiss[i].fd;
                pfd[n].events = POLLIN;
                map[n++] = i;
            }
            if (g_ctl[i] >= 0) {
                pfd[n].fd = g_ctl[i];
                pfd[n].events = POLLIN;
                map[n++] = MAX_CLIENTS + i;
            }
        }

        rc = poll(pfd, (nfds_t)n, 10);
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "poll: %s\n", strerror(errno));
            break;
        }

        for (int k = 0; k < n; k++) {
            if (!(pfd[k].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
                continue;
            if (map[k] == -1)
                accept_kiss();
            else if (map[k] == -2)
                accept_control();
            else if (map[k] == -3)
                read_stdin();
            else if (map[k] >= MAX_CLIENTS)
                read_control(map[k] - MAX_CLIENTS);
            else
                read_kiss(map[k]);
        }

        pump_tx();
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (g_kiss[i].used)
            close(g_kiss[i].fd);
        if (g_ctl[i] >= 0)
            close(g_ctl[i]);
    }
    close(g_kiss_listen);
    close(g_ctl_listen);

    logmsg("done: rx=%u sent=%u delivered=%u rejected=%u dup=%u full=%u",
           g_rx_frames, g_bridge.sent, g_delivered, g_rejected,
           g_bridge.dropped_dup, g_bridge.dropped_full);
    return 0;
}

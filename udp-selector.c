/**
 * UDP Multicast Selector
 *
 * Listens on multiple input multicast addresses and forwards packets
 * from the ACTIVE input to a single output multicast address.
 * Switching between inputs is instant (next packet from new source).
 *
 * Design: Zero-copy forwarding. Never transforms payload. Bit-exact.
 *
 * Control via Unix socket:
 *   echo "switch:0" | nc -U /tmp/udp-selector-<id>.sock
 *   echo "status" | nc -U /tmp/udp-selector-<id>.sock
 *   echo "quit" | nc -U /tmp/udp-selector-<id>.sock
 *
 * Build: make
 *   or:  gcc -O2 -o udp-selector udp-selector.c
 *
 * Usage: udp-selector <id> <output-mcast:port> <input1-mcast:port> [input2...] [--interface <ip>]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <inttypes.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_INPUTS 8
#define BUFFER_SIZE 65536
#define MAX_EVENTS 16
#define CONTROL_SOCK_PATH "/tmp/udp-selector-%s.sock"

// Kernel buffer sizes (2MB each)
#define SOCKET_RCVBUF_SIZE (2 * 1024 * 1024)
#define SOCKET_SNDBUF_SIZE (2 * 1024 * 1024)

// Control socket read timeout (ms)
#define CONTROL_READ_TIMEOUT_MS 100

// Error log throttling (max 1 per second)
#define ERROR_LOG_INTERVAL_SEC 1

typedef struct {
    int socket;
    struct sockaddr_in addr;
    char mcast_addr[64];
    uint64_t packets_received;
    uint64_t bytes_received;
    uint64_t recv_errors;
    time_t last_packet_time;
    time_t last_error_log_time;
} input_t;

typedef struct {
    char id[64];
    int epoll_fd;
    int control_socket;
    char control_path[128];

    // Multicast interface
    struct in_addr interface_addr;
    int interface_specified;

    // Output
    int output_socket;
    struct sockaddr_in output_addr;
    char output_mcast[64];
    uint64_t output_dropped_congestion;
    uint64_t output_send_errors;
    time_t last_output_error_log_time;

    // Inputs
    input_t inputs[MAX_INPUTS];
    int num_inputs;
    int active_input;  // 0-indexed

    // Stats
    uint64_t packets_forwarded;
    uint64_t bytes_forwarded;

    volatile int running;
} selector_t;

static selector_t g_selector;

void signal_handler(int sig) {
    (void)sig;
    g_selector.running = 0;
}

int parse_mcast_addr(const char *str, struct sockaddr_in *addr, char *out_str, size_t out_len) {
    char buf[64];
    strncpy(buf, str, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    char *colon = strchr(buf, ':');
    if (!colon) {
        fprintf(stderr, "Invalid address format (expected host:port): %s\n", str);
        return -1;
    }
    *colon = '\0';
    int port = atoi(colon + 1);

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    if (inet_pton(AF_INET, buf, &addr->sin_addr) != 1) {
        fprintf(stderr, "Invalid IP address: %s\n", buf);
        return -1;
    }

    if (out_str) {
        snprintf(out_str, out_len, "%s:%d", buf, port);
    }

    return 0;
}

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void set_socket_buffers(int sock, int is_output) {
    int rcvbuf = SOCKET_RCVBUF_SIZE;
    int sndbuf = SOCKET_SNDBUF_SIZE;

    if (!is_output) {
        if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
            perror("SO_RCVBUF");
        }
    }
    if (is_output) {
        if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf)) < 0) {
            perror("SO_SNDBUF");
        }
    }
}

int create_mcast_listener(selector_t *sel, const struct sockaddr_in *addr) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    set_socket_buffers(sock, 0);

    // STRICT BIND: Bind to the multicast address itself, not INADDR_ANY
    // This prevents receiving packets from other multicast groups on the same port
    // (the classic Linux multicast trap)
    if (bind(sock, (struct sockaddr *)addr, sizeof(*addr)) < 0) {
        perror("bind");
        close(sock);
        return -1;
    }

    struct ip_mreq mreq;
    mreq.imr_multiaddr = addr->sin_addr;
    mreq.imr_interface = sel->interface_specified ? sel->interface_addr : (struct in_addr){INADDR_ANY};

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) {
        perror("IP_ADD_MEMBERSHIP");
        close(sock);
        return -1;
    }

    if (set_nonblocking(sock) < 0) {
        perror("fcntl nonblock");
        close(sock);
        return -1;
    }

    return sock;
}

int create_mcast_sender(selector_t *sel, const struct sockaddr_in *addr) {
    (void)addr;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return -1;
    }

    set_socket_buffers(sock, 1);

    unsigned char ttl = 1;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));

    unsigned char loop = 0;
    setsockopt(sock, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));

    if (sel->interface_specified) {
        if (setsockopt(sock, IPPROTO_IP, IP_MULTICAST_IF, &sel->interface_addr, sizeof(sel->interface_addr)) < 0) {
            perror("IP_MULTICAST_IF");
            close(sock);
            return -1;
        }
    }

    // Non-blocking: drop rather than block
    if (set_nonblocking(sock) < 0) {
        perror("fcntl nonblock output");
        close(sock);
        return -1;
    }

    return sock;
}

int create_control_socket(const char *path) {
    int sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket (unix)");
        return -1;
    }

    unlink(path);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind (unix)");
        close(sock);
        return -1;
    }

    if (listen(sock, 5) < 0) {
        perror("listen");
        close(sock);
        return -1;
    }

    if (set_nonblocking(sock) < 0) {
        perror("fcntl nonblock control");
        close(sock);
        return -1;
    }

    return sock;
}

void handle_control_command(selector_t *sel, int client_fd) {
    struct timeval tv = { .tv_sec = 0, .tv_usec = CONTROL_READ_TIMEOUT_MS * 1000 };
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buf[256];
    ssize_t n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) {
        close(client_fd);
        return;
    }
    buf[n] = '\0';

    char *nl = strchr(buf, '\n');
    if (nl) *nl = '\0';

    char response[2048];

    if (strncmp(buf, "switch:", 7) == 0) {
        int new_active = atoi(buf + 7);
        if (new_active >= 0 && new_active < sel->num_inputs) {
            int old_active = sel->active_input;
            sel->active_input = new_active;
            snprintf(response, sizeof(response),
                "OK switched from input %d (%s) to input %d (%s)\n",
                old_active, sel->inputs[old_active].mcast_addr,
                new_active, sel->inputs[new_active].mcast_addr);
            fprintf(stderr, "[SELECTOR] %s", response);
        } else {
            snprintf(response, sizeof(response),
                "ERROR invalid input index: %d (valid: 0-%d)\n",
                new_active, sel->num_inputs - 1);
        }
    } else if (strcmp(buf, "status") == 0) {
        int offset = 0;
        offset += snprintf(response + offset, sizeof(response) - offset,
            "id: %s\n"
            "active_input: %d\n"
            "output: %s\n"
            "forwarded_packets: %" PRIu64 "\n"
            "forwarded_bytes: %" PRIu64 "\n"
            "output_dropped_congestion: %" PRIu64 "\n"
            "output_send_errors: %" PRIu64 "\n",
            sel->id, sel->active_input, sel->output_mcast,
            sel->packets_forwarded, sel->bytes_forwarded,
            sel->output_dropped_congestion, sel->output_send_errors);

        for (int i = 0; i < sel->num_inputs; i++) {
            input_t *inp = &sel->inputs[i];
            long last_pkt_age = inp->last_packet_time ? (long)(time(NULL) - inp->last_packet_time) : -1;
            offset += snprintf(response + offset, sizeof(response) - offset,
                "input[%d]: addr=%s packets=%" PRIu64 " bytes=%" PRIu64 " errors=%" PRIu64 " last_pkt=%lds%s\n",
                i, inp->mcast_addr, inp->packets_received, inp->bytes_received,
                inp->recv_errors, last_pkt_age,
                i == sel->active_input ? " [ACTIVE]" : "");
        }
    } else if (strcmp(buf, "quit") == 0) {
        snprintf(response, sizeof(response), "OK shutting down\n");
        sel->running = 0;
    } else {
        snprintf(response, sizeof(response),
            "ERROR unknown command. Commands: switch:<N>, status, quit\n");
    }

    write(client_fd, response, strlen(response));
    close(client_fd);
}

void run_selector(selector_t *sel) {
    struct epoll_event events[MAX_EVENTS];
    char buffer[BUFFER_SIZE];

    fprintf(stderr, "[SELECTOR] Running. Active input: %d (%s) -> Output: %s\n",
        sel->active_input, sel->inputs[sel->active_input].mcast_addr, sel->output_mcast);
    fprintf(stderr, "[SELECTOR] Control socket: %s\n", sel->control_path);
    fprintf(stderr, "[SELECTOR] Interface: %s\n",
        sel->interface_specified ? inet_ntoa(sel->interface_addr) : "any");
    fprintf(stderr, "[SELECTOR] Input indexing: 0-based (switch:0 = first input)\n");

    while (sel->running) {
        int nfds = epoll_wait(sel->epoll_fd, events, MAX_EVENTS, 100);

        for (int i = 0; i < nfds; i++) {
            int fd = events[i].data.fd;

            if (fd == sel->control_socket) {
                int client_fd = accept(sel->control_socket, NULL, NULL);
                if (client_fd >= 0) {
                    handle_control_command(sel, client_fd);
                }
                continue;
            }

            for (int j = 0; j < sel->num_inputs; j++) {
                if (fd == sel->inputs[j].socket) {
                    input_t *inp = &sel->inputs[j];
                    ssize_t n;

                    while ((n = recv(fd, buffer, sizeof(buffer), 0)) > 0) {
                        inp->packets_received++;
                        inp->bytes_received += n;
                        inp->last_packet_time = time(NULL);

                        if (j == sel->active_input) {
                            ssize_t sent = sendto(sel->output_socket, buffer, n, 0,
                                (struct sockaddr *)&sel->output_addr, sizeof(sel->output_addr));

                            if (sent < 0) {
                                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                    sel->output_dropped_congestion++;
                                } else {
                                    sel->output_send_errors++;
                                    time_t now = time(NULL);
                                    if (now - sel->last_output_error_log_time >= ERROR_LOG_INTERVAL_SEC) {
                                        fprintf(stderr, "[SELECTOR] sendto error: %s (total: %" PRIu64 ")\n",
                                            strerror(errno), sel->output_send_errors);
                                        sel->last_output_error_log_time = now;
                                    }
                                }
                            } else {
                                sel->packets_forwarded++;
                                sel->bytes_forwarded += sent;
                            }
                        }
                    }

                    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                        inp->recv_errors++;
                        time_t now = time(NULL);
                        if (now - inp->last_error_log_time >= ERROR_LOG_INTERVAL_SEC) {
                            fprintf(stderr, "[SELECTOR] recv error on input %d: %s (total: %" PRIu64 ")\n",
                                j, strerror(errno), inp->recv_errors);
                            inp->last_error_log_time = now;
                        }
                    }
                    break;
                }
            }
        }
    }

    fprintf(stderr, "[SELECTOR] Shutting down. Stats:\n");
    fprintf(stderr, "  Forwarded: %" PRIu64 " packets, %" PRIu64 " bytes\n",
        sel->packets_forwarded, sel->bytes_forwarded);
    fprintf(stderr, "  Dropped (congestion): %" PRIu64 "\n", sel->output_dropped_congestion);
    fprintf(stderr, "  Output errors: %" PRIu64 "\n", sel->output_send_errors);
}

void cleanup(selector_t *sel) {
    for (int i = 0; i < sel->num_inputs; i++) {
        if (sel->inputs[i].socket >= 0) {
            close(sel->inputs[i].socket);
        }
    }
    if (sel->output_socket >= 0) close(sel->output_socket);
    if (sel->control_socket >= 0) close(sel->control_socket);
    if (sel->epoll_fd >= 0) close(sel->epoll_fd);
    unlink(sel->control_path);
}

void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <id> <output-mcast:port> <input1-mcast:port> [input2...] [--interface <ip>]\n", prog);
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s stream1 239.0.0.1:5000 239.0.1.1:5001 239.0.1.2:5002 --interface 127.0.0.1\n", prog);
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --interface <ip>  Bind multicast to specific interface\n");
    fprintf(stderr, "\nControl commands (via unix socket):\n");
    fprintf(stderr, "  switch:<N>  Switch to input N (0-indexed)\n");
    fprintf(stderr, "  status      Show statistics\n");
    fprintf(stderr, "  quit        Shutdown\n");
}

int main(int argc, char *argv[]) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }

    memset(&g_selector, 0, sizeof(g_selector));
    g_selector.running = 1;
    g_selector.output_socket = -1;
    g_selector.control_socket = -1;
    g_selector.epoll_fd = -1;

    int arg_idx = 1;

    strncpy(g_selector.id, argv[arg_idx++], sizeof(g_selector.id) - 1);
    snprintf(g_selector.control_path, sizeof(g_selector.control_path), CONTROL_SOCK_PATH, g_selector.id);

    if (parse_mcast_addr(argv[arg_idx++], &g_selector.output_addr, g_selector.output_mcast, sizeof(g_selector.output_mcast)) < 0) {
        return 1;
    }

    while (arg_idx < argc) {
        if (strcmp(argv[arg_idx], "--interface") == 0) {
            if (arg_idx + 1 >= argc) {
                fprintf(stderr, "--interface requires an IP address\n");
                return 1;
            }
            if (inet_pton(AF_INET, argv[arg_idx + 1], &g_selector.interface_addr) != 1) {
                fprintf(stderr, "Invalid interface IP: %s\n", argv[arg_idx + 1]);
                return 1;
            }
            g_selector.interface_specified = 1;
            arg_idx += 2;
        } else {
            if (g_selector.num_inputs >= MAX_INPUTS) {
                fprintf(stderr, "Too many inputs (max %d)\n", MAX_INPUTS);
                return 1;
            }
            if (parse_mcast_addr(argv[arg_idx], &g_selector.inputs[g_selector.num_inputs].addr,
                    g_selector.inputs[g_selector.num_inputs].mcast_addr,
                    sizeof(g_selector.inputs[g_selector.num_inputs].mcast_addr)) < 0) {
                return 1;
            }
            g_selector.inputs[g_selector.num_inputs].socket = -1;
            g_selector.num_inputs++;
            arg_idx++;
        }
    }

    if (g_selector.num_inputs == 0) {
        fprintf(stderr, "At least one input required\n");
        print_usage(argv[0]);
        return 1;
    }

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    g_selector.epoll_fd = epoll_create1(0);
    if (g_selector.epoll_fd < 0) {
        perror("epoll_create1");
        return 1;
    }

    g_selector.output_socket = create_mcast_sender(&g_selector, &g_selector.output_addr);
    if (g_selector.output_socket < 0) {
        cleanup(&g_selector);
        return 1;
    }
    fprintf(stderr, "[SELECTOR] Output: %s\n", g_selector.output_mcast);

    for (int i = 0; i < g_selector.num_inputs; i++) {
        g_selector.inputs[i].socket = create_mcast_listener(&g_selector, &g_selector.inputs[i].addr);
        if (g_selector.inputs[i].socket < 0) {
            cleanup(&g_selector);
            return 1;
        }

        struct epoll_event ev;
        ev.events = EPOLLIN;
        ev.data.fd = g_selector.inputs[i].socket;
        epoll_ctl(g_selector.epoll_fd, EPOLL_CTL_ADD, g_selector.inputs[i].socket, &ev);

        fprintf(stderr, "[SELECTOR] Input %d: %s\n", i, g_selector.inputs[i].mcast_addr);
    }

    g_selector.control_socket = create_control_socket(g_selector.control_path);
    if (g_selector.control_socket < 0) {
        cleanup(&g_selector);
        return 1;
    }

    struct epoll_event ev;
    ev.events = EPOLLIN;
    ev.data.fd = g_selector.control_socket;
    epoll_ctl(g_selector.epoll_fd, EPOLL_CTL_ADD, g_selector.control_socket, &ev);

    run_selector(&g_selector);

    cleanup(&g_selector);
    return 0;
}

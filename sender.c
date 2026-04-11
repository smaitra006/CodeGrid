/*
 * sender.c  —  Grid Dispatcher / Console
 * ─────────────────────────────────────────────────────────────────────────────
 * Connects to any grid node; if that node is not the leader it will receive a
 * MSG_REDIRECT pointing to the current leader and transparently reconnect.
 *
 * If the connection drops mid-job (leader died + election happened), the
 * console prints a warning and offers to resubmit the last job automatically
 * once it reconnects to the new leader.
 * ─────────────────────────────────────────────────────────────────────────────
 */
#include "common.h"

#include <arpa/inet.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE_IDLE 0
#define STATE_BUSY 1
#define MAX_RECONNECT_TRIES 5
#define RECONNECT_DELAY_SEC 2

/* ── Path / payload remembered for resubmission ─────────────────────────── */
static char g_last_path[1024] = {0};
static char *g_last_payload = NULL;
static long g_last_payload_sz = 0;
static int g_last_is_project = 0;

/* ── Helpers ─────────────────────────────────────────────────────────────── */
static void clean_path(char *path) {
  int len = (int)strlen(path);
  while (len > 0 && (path[len - 1] == '\n' || path[len - 1] == '\r' ||
                     path[len - 1] == ' '))
    path[--len] = '\0';
  char *start = path;
  while (*start == ' ')
    start++;
  if ((start[0] == '\'' && start[len - 1] == '\'') ||
      (start[0] == '\"' && start[len - 1] == '\"')) {
    start[len - 1] = '\0';
    start++;
  }
  memmove(path, start, strlen(start) + 1);
  char *rd = path, *wr = path;
  while (*rd) {
    if (*rd == '\\' && *(rd + 1) == ' ')
      rd++;
    *wr++ = *rd++;
  }
  *wr = '\0';
}

/*
 * Connect to ip:PORT, perform auth handshake, and handle redirects.
 * Returns a connected+authenticated socket fd, or -1 on total failure.
 */
static int dial_leader(const char *initial_ip) {
  char cur_ip[64];
  strncpy(cur_ip, initial_ip, sizeof cur_ip - 1);

  for (int attempt = 0; attempt < MAX_RECONNECT_TRIES; attempt++) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in srv = {.sin_family = AF_INET, .sin_port = htons(PORT)};
    inet_pton(AF_INET, cur_ip, &srv.sin_addr);

    if (connect(fd, (struct sockaddr *)&srv, sizeof srv) < 0) {
      close(fd);
      printf(C_YELLOW
             "[Reconnect] Could not reach %s, retrying in %ds...\n" C_RESET,
             cur_ip, RECONNECT_DELAY_SEC);
      sleep(RECONNECT_DELAY_SEC);
      continue;
    }

    /* Send auth */
    send_msg(fd, MSG_AUTH, "HELLO", 5);

    /* Read response — may be MSG_REDIRECT or the node just stays silent
       (original server.c never replied to MSG_AUTH, so we only consume
        a redirect if one arrives within a short timeout). */
    struct pollfd pf = {fd, POLLIN, 0};
    if (poll(&pf, 1, 1500) > 0 && (pf.revents & POLLIN)) {
      MsgHeader hdr;
      if (recv_hdr(fd, &hdr) < 0) {
        close(fd);
        continue;
      }

      if (hdr.type == MSG_REDIRECT) {
        RedirectMsg redir;
        if (hdr.payload_len >= sizeof redir)
          recv_all(fd, &redir, sizeof redir);
        close(fd);

        if (strcmp(redir.leader_ip, "unknown") == 0) {
          printf(C_YELLOW "[Grid] No leader elected yet. Waiting...\n" C_RESET);
          sleep(RECONNECT_DELAY_SEC);
          /* Retry same node — election may have just finished */
          continue;
        }
        printf(C_CYAN "[Grid] Redirected to leader at %s\n" C_RESET,
               redir.leader_ip);
        strncpy(cur_ip, redir.leader_ip, sizeof cur_ip - 1);
        continue; /* loop: connect to leader */

      } else if (hdr.type == MSG_REJECTED) {
        char *buf = hdr.payload_len ? malloc(hdr.payload_len + 1) : NULL;
        if (buf) {
          recv_all(fd, buf, hdr.payload_len);
          buf[hdr.payload_len] = '\0';
        }
        printf(C_RED "[Grid] Rejected: %s\n" C_RESET, buf ? buf : "");
        if (buf)
          free(buf);
        close(fd);
        return -1; /* banned or hard rejection */
      } else {
        /* Some other message arrived first (e.g. initial stream).
           Put it back? We can't — just handle it in the main loop. */
        /* For now: skip it and proceed */
        if (hdr.payload_len) {
          char *tmp = malloc(hdr.payload_len);
          recv_all(fd, tmp, hdr.payload_len);
          free(tmp);
        }
      }
    }
    /* Connected and authenticated */
    printf(C_BG_BLUE C_BOLD " GRID CONSOLE CONNECTED | LEADER: %s " C_RESET
                            "\n",
           cur_ip);
    strncpy(cur_ip, cur_ip, sizeof cur_ip);
    return fd;
  }

  printf(C_RED "[Grid] Failed to connect after %d attempts.\n" C_RESET,
         MAX_RECONNECT_TRIES);
  return -1;
}

/* ══════════════════════════════════════════════════════════════════════════
   main
   ══════════════════════════════════════════════════════════════════════════ */
int main(void) {
  char initial_ip[64] = {0};

  printf(C_CYAN C_BOLD
         "\n======================================================\n"
         "     [ DISPATCHER V9.0 ] Grid P2P Console        \n"
         "======================================================\n\n" C_RESET);
  printf(C_CYAN C_BOLD "Welcome to Grid Console.\n" C_RESET);
  printf("Enter any node IP (e.g., 127.0.0.1): ");
  if (fgets(initial_ip, sizeof initial_ip, stdin))
    initial_ip[strcspn(initial_ip, "\n")] = '\0';
  if (strlen(initial_ip) == 0)
    strcpy(initial_ip, "127.0.0.1");

  int sockfd = dial_leader(initial_ip);
  if (sockfd < 0)
    return 1;

  printf(C_GREEN "Usage: Drag a .c file OR a project folder here and hit "
                 "Enter!\n" C_RESET);

  int state = STATE_IDLE;

reconnect_loop:;
  struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {sockfd, POLLIN, 0}};

  while (1) {
    if (state == STATE_IDLE) {
      printf(C_MAGENTA "\nGRID> " C_RESET);
      fflush(stdout);
    }

    if (poll(fds, 2, -1) < 0)
      break;

    /* ── stdin input ───────────────────────────────────────────────── */
    if (fds[0].revents & POLLIN) {
      char input_buf[1024];
      ssize_t bytes = read(STDIN_FILENO, input_buf, sizeof(input_buf) - 1);
      if (bytes <= 0)
        break;
      input_buf[bytes] = '\0';

      if (state == STATE_IDLE) {
        clean_path(input_buf);
        if (strlen(input_buf) == 0)
          continue;
        if (!strcmp(input_buf, "exit") || !strcmp(input_buf, "quit"))
          break;

        struct stat st;
        if (stat(input_buf, &st) != 0) {
          printf(C_RED "Error: '%s' not found\n" C_RESET, input_buf);
          continue;
        }

        /* Remember path for potential resubmission */
        strncpy(g_last_path, input_buf, sizeof g_last_path - 1);
        if (g_last_payload) {
          free(g_last_payload);
          g_last_payload = NULL;
        }

        if (S_ISDIR(st.st_mode)) {
          printf(C_YELLOW "Packaging project directory...\n" C_RESET);
          char cmd[1024];
          snprintf(cmd, sizeof cmd,
                   "tar -czf /tmp/grid_send.tar.gz -C \"%s\" .", input_buf);
          system(cmd);
          FILE *src = fopen("/tmp/grid_send.tar.gz", "rb");
          if (!src) {
            printf(C_RED "Error packaging project.\n" C_RESET);
            continue;
          }
          fseek(src, 0, SEEK_END);
          long sz = ftell(src);
          rewind(src);
          g_last_payload = calloc(1, sz + 1);
          fread(g_last_payload, 1, sz, src);
          fclose(src);
          g_last_payload_sz = sz;
          g_last_is_project = 1;

          printf(C_CYAN "Dispatching Project (%ld bytes)...\n" C_RESET, sz);
          send_msg(sockfd, MSG_PROJECT_REQ, g_last_payload, (uint32_t)sz);
          state = STATE_BUSY;

        } else {
          FILE *src = fopen(input_buf, "rb");
          if (!src) {
            printf(C_RED "Error reading '%s'\n" C_RESET, input_buf);
            continue;
          }
          fseek(src, 0, SEEK_END);
          long sz = ftell(src);
          rewind(src);
          g_last_payload = calloc(1, sz + 1);
          fread(g_last_payload, 1, sz, src);
          fclose(src);
          g_last_payload_sz = sz;
          g_last_is_project = 0;

          printf(C_CYAN "Dispatching file (%ld bytes)...\n" C_RESET, sz);
          send_msg(sockfd, MSG_EXEC_REQ, g_last_payload, (uint32_t)sz);
          state = STATE_BUSY;
        }

      } else { /* STATE_BUSY — forward stdin to job */
        send_msg(sockfd, MSG_STREAM_IN, input_buf, (uint32_t)bytes);
      }
    }

    /* ── network response ──────────────────────────────────────────── */
    if (fds[1].revents & POLLIN) {
      MsgHeader hdr;
      if (recv_hdr(sockfd, &hdr) < 0) {
        /* Connection dropped — leader may have died */
        printf(C_RED "\n[Grid] Disconnected from leader!\n" C_RESET);

        if (state == STATE_BUSY) {
          printf(C_YELLOW
                 "[Grid] A job was running. Attempting reconnect...\n" C_RESET);
          close(sockfd);
          sleep(RECONNECT_DELAY_SEC + 2); /* wait for election to settle */
          sockfd = dial_leader(initial_ip);
          if (sockfd < 0)
            break;

          /* Offer automatic resubmission */
          if (g_last_payload && g_last_payload_sz > 0) {
            printf(C_YELLOW "[Grid] Reconnected to new leader.\n"
                            "       Last job: %s\n"
                            "       Resubmit automatically? [y/n]: " C_RESET,
                   g_last_path);
            fflush(stdout);
            char ans[8] = {0};
            if (fgets(ans, sizeof ans, stdin) &&
                (ans[0] == 'y' || ans[0] == 'Y')) {
              printf(C_CYAN "Resubmitting job...\n" C_RESET);
              MsgType req = g_last_is_project ? MSG_PROJECT_REQ : MSG_EXEC_REQ;
              send_msg(sockfd, req, g_last_payload,
                       (uint32_t)g_last_payload_sz);
              state = STATE_BUSY;
            } else {
              state = STATE_IDLE;
            }
            fds[1].fd = sockfd;
            goto reconnect_loop;
          }
          state = STATE_IDLE;
          fds[1].fd = sockfd;
          goto reconnect_loop;
        }
        break;
      }

      char *out_buf = NULL;
      if (hdr.payload_len > 0) {
        out_buf = malloc(hdr.payload_len + 1);
        recv_all(sockfd, out_buf, hdr.payload_len);
        out_buf[hdr.payload_len] = '\0';
      }

      switch ((MsgType)hdr.type) {
      case MSG_STREAM_OUT:
        if (out_buf) {
          printf("%s", out_buf);
          fflush(stdout);
        }
        break;
      case MSG_JOB_DONE:
        printf(C_GREEN "\n[Job Complete]\n" C_RESET);
        state = STATE_IDLE;
        break;
      case MSG_EXEC_RESULT:
        if (out_buf)
          printf(C_RED "\n[Grid Message]: %s\n" C_RESET, out_buf);
        state = STATE_IDLE;
        break;
      case MSG_STRIKE:
        if (out_buf)
          printf(C_YELLOW "\n[SECURITY STRIKE]: %s\n" C_RESET, out_buf);
        state = STATE_IDLE;
        break;
      case MSG_REJECTED:
        if (out_buf)
          printf(C_RED "\n[Grid Rejected]: %s\n" C_RESET, out_buf);
        if (out_buf && strstr(out_buf, "BANNED")) {
          free(out_buf);
          exit(1);
        }
        state = STATE_IDLE;
        break;
      case MSG_REDIRECT: {
        /* We may get a redirect if our leader just stepped down */
        RedirectMsg *redir = (RedirectMsg *)out_buf;
        if (redir && strcmp(redir->leader_ip, "unknown") != 0) {
          printf(C_YELLOW
                 "\n[Grid] Leader changed, reconnecting to %s...\n" C_RESET,
                 redir->leader_ip);
          close(sockfd);
          sockfd = dial_leader(redir->leader_ip);
          if (sockfd < 0) {
            if (out_buf)
              free(out_buf);
            goto done;
          }
          state = STATE_IDLE;
          fds[1].fd = sockfd;
          if (out_buf)
            free(out_buf);
          goto reconnect_loop;
        }
        break;
      }
      default:
        break;
      }
      if (out_buf)
        free(out_buf);
    }
  }

done:
  close(sockfd);
  if (g_last_payload)
    free(g_last_payload);
  return 0;
}

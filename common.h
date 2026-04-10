#ifndef COMMON_H
#define COMMON_H

#include <stdint.h>
#include <stdio.h>
#include <sys/socket.h>
#include <sys/types.h>

#define C_RESET "\x1b[0m"
#define C_BOLD "\x1b[1m"
#define C_RED "\x1b[31m"
#define C_GREEN "\x1b[32m"
#define C_YELLOW "\x1b[33m"
#define C_BLUE "\x1b[34m"
#define C_MAGENTA "\x1b[35m"
#define C_CYAN "\x1b[36m"
#define C_BG_BLUE "\x1b[44m"

#define PORT 8080
#define MCAST_PORT 9090
#define MCAST_GROUP "239.0.0.1"
#define MAX_CLIENTS 64
#define MAX_THREADS 64
#define MAX_RESULT_TEXT (64 * 1024)
#define MAX_QUEUE_SIZE 100
#define AUTH_TOKEN "N1T_CSE_SECURE_77"
#define P2P_TOKEN "GRID_P2P_NODE_AUTH"

typedef enum {
  MSG_CPU_REPORT = 1,
  MSG_EXEC_REQ = 2,
  MSG_EXEC_WORK = 3,
  MSG_EXEC_RESULT = 4,
  MSG_AUTH = 5,
  MSG_REJECTED = 6,
  MSG_STREAM_IN = 7,
  MSG_STREAM_OUT = 8,
  MSG_JOB_DONE = 9,
  MSG_STRIKE = 10,
  MSG_PROJECT_REQ = 11,
  MSG_PROJECT_WORK = 12,
  MSG_PEER_HELLO = 13,
  MSG_HEARTBEAT = 14,
  MSG_JOB_ASSIGN = 15,  /* Originator -> Worker */
  MSG_TAGGED_OUT = 16,  /* Worker -> Originator */
  MSG_TAGGED_DONE = 17, /* Worker -> Originator */
  MSG_TAGGED_ERR = 18   /* Worker -> Originator */
} MsgType;

#pragma pack(push, 1)

typedef struct {
  uint8_t type;
  uint32_t payload_len;
  char auth_token[32];
} MsgHeader;

typedef struct {
  uint32_t num_threads;
  uint32_t active_jobs; /* New: Track load capacity accurately */
  float usage[MAX_THREADS];
} CpuReport;

typedef struct {
  uint32_t node_id;
  char ip[16];
} NodeInfoMsg;

typedef struct {
  uint64_t job_id;
  uint8_t job_type;
  uint32_t code_len;
} JobAssignMsg;

typedef struct {
  uint64_t job_id;
} TaggedHdr;

typedef struct {
  uint32_t node_id;
  char ip[16];
} BeaconMsg;

#pragma pack(pop)

static inline int send_all(int fd, const void *buf, size_t len) {
  const char *p = (const char *)buf;
  while (len > 0) {
    ssize_t n = send(fd, p, len, 0);
    if (n <= 0)
      return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static inline int recv_all(int fd, void *buf, size_t len) {
  char *p = (char *)buf;
  while (len > 0) {
    ssize_t n = recv(fd, p, len, 0);
    if (n <= 0)
      return -1;
    p += n;
    len -= (size_t)n;
  }
  return 0;
}

static inline int send_msg(int fd, MsgType type, const void *payload,
                           uint32_t plen) {
  MsgHeader hdr = {(uint8_t)type, plen, ""};
  snprintf(hdr.auth_token, sizeof(hdr.auth_token), "%s", AUTH_TOKEN);
  if (send_all(fd, &hdr, sizeof hdr) < 0)
    return -1;
  if (plen && payload && send_all(fd, payload, plen) < 0)
    return -1;
  return 0;
}

static inline int send_p2p(int fd, MsgType type, const void *payload,
                           uint32_t plen) {
  MsgHeader hdr = {(uint8_t)type, plen, ""};
  snprintf(hdr.auth_token, sizeof(hdr.auth_token), "%s", P2P_TOKEN);
  if (send_all(fd, &hdr, sizeof hdr) < 0)
    return -1;
  if (plen && payload && send_all(fd, payload, plen) < 0)
    return -1;
  return 0;
}

static inline int recv_hdr(int fd, MsgHeader *hdr) {
  return recv_all(fd, hdr, sizeof *hdr);
}

#endif /* COMMON_H */

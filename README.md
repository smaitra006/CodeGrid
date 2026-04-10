# 🚀 CodeGrid — Decentralized Fault-Tolerant Distributed Execution Engine

> A fully decentralized, self-healing, peer-to-peer distributed compute grid where **every node is both a worker and a server**, capable of **leader election, job migration, and real-time execution streaming**.

---

## 🌍 Why This Project Exists

Modern distributed systems (like Kubernetes, Spark, Ray) rely heavily on:

- Centralized schedulers
- Complex orchestration layers
- Heavy infrastructure dependencies

### ❌ Problems with Traditional Systems

- Single point of failure (leader/master node)
- Complex setup (clusters, configs, orchestration)
- Poor resilience in dynamic environments

---

### ✅ Vision of GRID V9

> Build a **zero-dependency, plug-and-play distributed compute system** where:

- Every node is **equal**
- Leadership is **dynamic**
- Failures are **handled automatically**
- Jobs are **never lost**

---

## 🧠 High-Level Concept

GRID is essentially a:

> **Self-organizing distributed execution network**
> where nodes discover each other, elect a leader, distribute workloads, and recover from failures — automatically.

---

## 🏗️ System Overview

### 🔹 Core Components

1. **Grid Node (`node.c`)**
2. **Dispatcher / CLI Client (`sender.c`)**
3. **Web Interface (`server.js`)**
4. **Communication Protocol (`common.h`)**

---

## ⚙️ Architecture

```
                    ┌──────────────────────┐
                    │   Web UI (Node.js)   │
                    │   Socket.IO + REST   │
                    └─────────┬────────────┘
                              │
                              ▼
                    ┌──────────────────────┐
                    │   Dispatcher (C)     │
                    │   sender.c           │
                    └─────────┬────────────┘
                              │
                ┌─────────────┼─────────────┐
                ▼             ▼             ▼
        ┌────────────┐ ┌────────────┐ ┌────────────┐
        │  Node A    │ │  Node B    │ │  Node C    │
        │ Leader     │ │ Worker     │ │ Worker     │
        └────────────┘ └────────────┘ └────────────┘
                ▲             ▲             ▲
                └────── P2P Mesh Network ──┘
```

---

## 🔄 Core Design Philosophy

### 🧩 1. Fully Decentralized System

- No fixed master node
- Every node:
  - Can become leader
  - Can execute jobs

- Achieved using **Bully Election Algorithm**

📄 See:

---

### ⚡ 2. Leader-Based Coordination (Dynamic)

- Leader responsibilities:
  - Job scheduling
  - Load balancing
  - Failure recovery

- Leader is:
  - Automatically elected
  - Automatically replaced

---

### 🔍 3. Peer Discovery via Multicast

- UDP Multicast (`239.0.0.1:9090`)
- Nodes broadcast:
  - IP
  - Node ID

📄 Defined in:

---

### 🔁 4. Self-Healing System

| Failure                | System Reaction           |
| ---------------------- | ------------------------- |
| Worker dies            | Job reassigned            |
| Leader dies            | Election triggered        |
| Dispatcher disconnects | Jobs cancelled or resumed |

---

## 🧱 Deep Architecture Breakdown

---

### 🧠 Node (`node.c`)

> The **brain of the system**

#### Roles:

- `FOLLOWER`
- `CANDIDATE`
- `LEADER`

#### Responsibilities:

✔ Peer discovery
✔ Leader election
✔ Load balancing
✔ Job execution
✔ Failure recovery

---

### ⚡ Job Lifecycle

```
Dispatcher → Leader → Worker → Leader → Dispatcher
```

---

### 🔄 Job Flow

1. Dispatcher sends job
2. Leader:
   - Scans for malware
   - Picks least-loaded worker

3. Worker executes
4. Output streamed back in real-time
5. Completion / error handled

---

### 📊 Load Balancing Strategy

```c
find_best_worker_idx()
```

- Based on **CPU utilization**
- Chooses least-loaded node

📄 Source:

---

### 🧵 Concurrency Model

- Multi-threaded:
  - Peer threads
  - Dispatcher threads
  - Job monitor threads

- Uses:
  - `pthread`
  - `epoll`
  - non-blocking IO

---

## 🗳️ Leader Election (Bully Algorithm)

### 💡 Why Bully?

- Simple
- Deterministic
- Fast convergence

---

### ⚙️ How It Works

1. Node detects leader failure
2. Sends election message to higher-ID nodes
3. If no response → becomes leader
4. Broadcasts leadership

---

### 📌 Key Insight

> Node ID = IP address → guarantees uniqueness

---

## 🔐 Security Design

### 🚫 Malware Detection

```c
scan_for_malware()
```

Blocks:

- `system()`
- `execvp`
- `unlink`
- etc.

---

### ⚠️ Strike System

| Strikes | Action        |
| ------- | ------------- |
| 1–2     | Warning       |
| 3       | Permanent ban |

---

### 🔑 Authentication

Two tokens:

- Dispatcher → Node: `AUTH_TOKEN`
- Node ↔ Node: `P2P_TOKEN`

📄 Defined in:

---

## 📦 Job Types

### 1️⃣ Single File Execution

- Input: `.c` file
- Compiled using `gcc`

---

### 2️⃣ Project Execution

- Input: folder
- Compressed → sent → extracted → compiled

---

## 🔁 Fault Tolerance (Key Highlight)

---

### 💥 Case 1: Worker Dies

✔ Job automatically reassigned
✔ Dispatcher sees no interruption

---

### 💥 Case 2: Leader Dies

✔ Election triggered
✔ New leader takes over
✔ Workers report active jobs

---

### 💥 Case 3: Dispatcher Disconnect

✔ Reconnect + optional resubmission

📄 See:

---

## 🌐 Web Interface (`server.js`)

> Modern developer UX layer

---

### Features:

- Run code from browser
- Upload files/folders
- Live terminal streaming
- Interactive input support

---

### Tech Stack:

- Express.js
- Socket.IO
- Child process integration

📄 Source:

---

## 💻 Dispatcher (`sender.c`)

### Features:

- Auto leader discovery
- Transparent redirection
- Auto reconnect
- Job resubmission

---

### UX Example

```
GRID> ./hello.c
Dispatching file...
[Grid]: Worker assigned...
Hello World
[Job Complete]
```

---

## 📡 Communication Protocol

Defined in:

📄

---

### Message Types

| Type             | Purpose        |
| ---------------- | -------------- |
| `MSG_EXEC_REQ`   | Execute code   |
| `MSG_JOB_ASSIGN` | Assign job     |
| `MSG_TAGGED_OUT` | Stream output  |
| `MSG_HEARTBEAT`  | Node liveness  |
| `MSG_ELECTION`   | Start election |

---

### Wire Format

```c
typedef struct {
  uint8_t type;
  uint32_t payload_len;
  char auth_token[32];
} MsgHeader;
```

---

## 📊 Observability

### 🖥️ TUI Dashboard

- Live peers
- CPU usage
- Leader status
- Job queue
- Logs

---

### 📄 Ledger System

- CSV logging
- Tracks:
  - Elections
  - Failures
  - Security events

---

## ⚡ Performance Considerations

- Zero-copy streaming (pipes)
- Parallel execution
- Efficient scheduling
- Minimal overhead protocol

---

## 🧪 Example Use Cases

- Distributed code execution
- Remote compilation grid
- Competitive programming cluster
- Lightweight CI/CD system
- Fault-tolerant compute backend

---

## 🛠️ Build & Run

### 🔧 Compile

```bash
make
```

---

### ▶️ Start Nodes

```bash
./node
```

(Start multiple instances)

---

### 🧑‍💻 Run Dispatcher

```bash
./sender
```

---

### 🌐 Run Web UI

```bash
node server.js
```

---

## 🔮 Future Improvements

- Multi-language support (Python, Java)
- Docker sandboxing
- Distributed file system
- Persistent job recovery
- Metrics dashboard (Prometheus/Grafana)

---

## 🏆 What Makes This Project Stand Out

✔ Fully decentralized architecture
✔ Real-time distributed execution
✔ Strong fault tolerance
✔ Clean protocol design
✔ Systems-level depth (OS + Networks + Distributed Systems)

---

## 🧠 What You Demonstrate With This

- Distributed systems mastery
- Networking (TCP, UDP, sockets)
- OS concepts (fork, pipes, scheduling)
- Concurrency (threads, locks)
- System design thinking
- Production-grade engineering

---

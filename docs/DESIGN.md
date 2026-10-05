# Concurrent Encryption Service on QNX: Design

A client sends a **Data Block** (up to 2 MB) to a QNX server. The server encrypts the block's **Segments** in parallel on a **Worker Pool**, uses a mutex to protect the shared output, signals completion with a condition variable, and returns the **Encrypted Block** asynchronously. The terms in bold are defined in [`CONTEXT.md`](../CONTEXT.md).

- Target: QNX SDP 8.0.3 on a Raspberry Pi 4B (4 × Cortex-A72, no AES hardware instructions).
- Cipher: AES-256-CTR.

---

## 1. Architecture

```
 CLIENT PROCESS (encclient)                    SERVER PROCESS (encserver)
┌─────────────────────────┐          ┌───────────────────────────────────────────────┐
│ Main thread             │          │ Receive Thread  (MsgReceive loop)             │
│  ├─ starts sender thread│          │   ├─ waits for a free Job slot (max 8)        │
│  ├─ stays free          │          │   ├─ receives the 8-byte request header       │
│  └─ woken by done_cv    │          │   ├─ MsgRead pulls the Data Block             │
│                         │ MsgSendv │   ├─ creates the Job, keeps rcvid             │
│ Sender thread  ─────────┼─────────►│   └─ starts a Job Coordinator, loops again    │
│  └─ REPLY-blocked       │          │                                               │
│                         │          │ Job Coordinator (one per Job)                 │
│                         │          │   ├─ splits Data Block into Segments          │
│                         │          │   ├─ queues one Task per Segment ──────┐      │
│                         │          │   ├─ waits on job.done_cv ◄── signal ─┐│      │
│                         │MsgReplyv │   └─ replies with Encrypted Block     ││      │
│  ◄──────────────────────┼──────────┤                                       ││      │
│ verifies the result     │          │ Worker Pool  (-w N, default = CPUs)   ││      │
└─────────────────────────┘          │   queue [queue_mutex + not_empty_cv]◄─┘│      │
                                     │   worker: encrypt into private buffer, │      │
                                     │     lock job.mutex, copy into output,  │      │
                                     │     count++, last one signals ─────────┘      │
                                     └───────────────────────────────────────────────┘
```

| Component | Responsibility | Code |
|---|---|---|
| Receive Thread | Owns the channel. Admits at most `ENC_MAX_JOBS` Jobs, pulls the Data Block, starts a Job, and never waits for one. | `src/qnx/server.c` `receive_loop`, `handle_encrypt` |
| Job Coordinator | Runs one Job and sends the deferred reply. | `src/qnx/server.c` `job_coordinator`; `src/core/job.c` `job_run` |
| Worker Pool | A fixed set of threads that take Tasks from one shared FIFO queue. | `src/core/worker_pool.c` |
| Task | Encrypts one Segment, then commits it under the Job mutex. | `src/core/job.c` `encrypt_segment`, `commit_segment` |
| Segment planning | Decides the Segment size for a Data Block. | `src/core/segment.c` |
| Cipher | AES-256-CTR that can start at any block-aligned offset. | `src/core/cipher_*.c` |
| Client | Sends asynchronously, verifies the result, and reports times. | `src/qnx/client.c` |

---

## 2. Key decisions

| # | Decision | Reason |
|---|---|---|
| 1 | **Deferred reply**: the server keeps the `rcvid` and the Job Coordinator replies when the Job is done. The client blocks only its sender thread. | Keeps one request/response cycle and keeps the server responsive. See [ADR 0001](adr/0001-deferred-reply.md). |
| 2 | **Large messages**: the client sends `[header, Data Block]` as two iov parts. The server receives only the header, then pulls the Data Block with `MsgRead` at offset `sizeof(header)`. The reply is `[reply header, Encrypted Block]` via `MsgReplyv`. | No receive buffer has to be as large as the Data Block, and no extra copy is made into one contiguous reply buffer. |
| 3 | **AES-256-CTR**, with each Segment starting at counter `IV + offset/16`. | CTR has no chaining between blocks, so Segments are independent and can run in parallel. The output is the same length as the input. |
| 4 | **New random IV for every Job**, returned in the reply. | Reusing an IV with the same key in CTR mode exposes the data. |
| 5 | **Shared key file** that the client and server both load at start-up (`enc.key`, 32 bytes). | The client needs the key to verify. Key handling stays out of the protocol. |
| 6 | **Workers encrypt into private memory** and take `job.mutex` only to copy into the output and update the counter. | The critical section is one `memcpy`, so workers rarely wait on each other. |
| 7 | **Predicate wait**: `while completed < total: wait(done_cv)`. | Handles spurious wake-ups, and the case where the last worker finishes before the Coordinator waits. |
| 8 | **At most 8 Jobs** in progress. The Receive Thread stops calling `MsgReceive` while 8 are open. | Extra clients wait send-blocked in the kernel, with no extra queue code. Memory stays at or below about 32 MB. |
| 9 | **Abandoned Job**: if the client goes away, the Tasks finish and `MsgReplyv` fails with `ESRCH`, so the result is discarded. | Simple and safe. Cancellation would need unblock-pulse handling and more locking. |
| 10 | **Size rules**: a Data Block of 0 bytes is rejected (`EINVAL`), more than 2 MB is rejected (`EMSGSIZE`), and 64 KB or less is one Segment. | A Job with zero Segments would never signal completion. Splitting tiny blocks only adds overhead. |

### Segment size

```
if len ≤ 64 KB:           one Segment of len bytes
else:
    target   = ceil(len / (workers × 4))     // 4 Tasks per worker balances the load
    seg_size = round_up(max(16 KB, target), 16)
```

A 2 MB Data Block on 4 workers gives 16 Segments of 128 KB.

---

## 3. Data structures

```
STRUCT Task                         // one per Segment; queued on the Worker Pool
    job, offset, length

STRUCT Job
    key, iv, input, len, total_segments      // read-only while the Job runs
    output, completed_count, error           // shared: guarded by mutex
    finish_time                              // set by the worker that completes the last Segment
    mutex, done_cv

STRUCT WorkerPool
    threads[N]
    queue (FIFO of Task), shutdown           // guarded by queue_mutex
    queue_mutex, not_empty_cv
```

---

## 4. Pseudocode

### 4.1 Worker Pool creation

```
FUNCTION pool_create(n)
    pool.queue <- empty; pool.shutdown <- false
    INIT queue_mutex, not_empty_cv
    REPEAT n TIMES: START THREAD worker_loop(pool)

FUNCTION pool_submit(pool, task)
    LOCK queue_mutex
        ENQUEUE task
        SIGNAL not_empty_cv
    UNLOCK queue_mutex

FUNCTION worker_loop(pool)
    LOOP
        LOCK queue_mutex
            WHILE queue empty AND NOT shutdown: WAIT(not_empty_cv, queue_mutex)
            IF queue empty: UNLOCK; EXIT THREAD          // shutdown and drained
            task <- DEQUEUE
        UNLOCK queue_mutex
        encrypt_segment(task)                            // no pool lock held

FUNCTION pool_destroy(pool)
    LOCK queue_mutex; shutdown <- true; BROADCAST not_empty_cv; UNLOCK
    JOIN all threads
```

### 4.2 Segment division

```
FUNCTION split(job, workers)
    seg <- segment_size_for(job.len, workers)            // see "Segment size" above
    FOR offset FROM 0 TO job.len STEP seg
        ADD Task{ job, offset, MIN(seg, job.len - offset) }
    job.total_segments <- number of Tasks                // set before any submit
```

### 4.3 Encryption of each Segment

```
FUNCTION encrypt_segment(task)
    scratch <- ALLOCATE(task.length)                     // private to this worker
    status  <- AES256_CTR(key, counter = job.iv + task.offset / 16,
                          job.input[task.offset ..], scratch)
    commit_segment(task, scratch, status)
```

### 4.4 Mutex protection of the output buffer

```
FUNCTION commit_segment(task, scratch, status)
    LOCK job.mutex
        IF status OK: COPY scratch -> job.output[task.offset ..]
        ELSE IF job.error unset: job.error <- status
        job.completed_count += 1                         // counted even on failure
        IF job.completed_count = job.total_segments
            job.finish_time <- NOW
            SIGNAL job.done_cv
    UNLOCK job.mutex
```

### 4.5 Condition-variable signalling when all Tasks finish

```
FUNCTION job_run(job, pool)                              // Job Coordinator thread
    split(job, pool.size)
    start <- NOW
    FOR EACH task: pool_submit(pool, task)
    LOCK job.mutex
        WHILE job.completed_count < job.total_segments: WAIT(job.done_cv, job.mutex)
        job.encryption_time <- job.finish_time - start
    UNLOCK job.mutex
    RETURN job.error
```

### 4.6 Asynchronous return of the Encrypted Block

```
// Server: Receive Thread
LOOP
    WAIT until active_jobs < 8; active_jobs += 1
    rcvid, header <- MsgReceive(chid)                    // header only
    IF pulse / connect / unknown type: handle; active_jobs -= 1; CONTINUE
    VALIDATE header.data_len (1 .. 2 MB) ELSE MsgError
    job.input <- MsgRead(rcvid, data_len, offset = sizeof header)
    job.iv    <- RANDOM 16 bytes
    START DETACHED THREAD job_coordinator(job, rcvid)    // no reply here: deferred
    // back to MsgReceive immediately

// Server: Job Coordinator
err <- job_run(job, pool)
IF err: MsgError(rcvid, err)
ELSE:   MsgReplyv(rcvid, [reply_header{len, segments, iv, encryption_time, workers},
                          job.output])
        IF that fails (ESRCH): Abandoned Job, discard
FREE job; active_jobs -= 1; SIGNAL slot_free_cv

// Client
START THREAD sender:
    MsgSendv(coid, [header, data], [reply_header, encrypted])   // blocks this thread only
    LOCK; done <- true; SIGNAL done_cv; UNLOCK
Main thread: free to work; then WAIT on done_cv until done
VERIFY: encrypted = ReferenceEncryption(data) AND decrypt(encrypted) = data
```

---

## 5. Communication flow

- The server starts, loads `enc.key`, runs the cipher self-test, starts N workers, and registers `encsvc` with `name_attach`.
- The client loads the same `enc.key` and connects with `name_open("encsvc")`.
- The client's sender thread calls `MsgSendv([header | Data Block])` and becomes reply-blocked. The client's main thread keeps running.
- The Receive Thread takes a Job slot, receives the header, checks the size, and pulls the Data Block with `MsgRead`.
- The Receive Thread creates the Job with a fresh IV, keeps the `rcvid`, starts a Job Coordinator, and returns to `MsgReceive`.
- The Coordinator splits the Data Block into Segments and queues one Task per Segment.
- Workers encrypt their Segments in parallel, each in private memory.
- Each worker locks `job.mutex`, copies its Segment into the Encrypted Block, increments the counter, and unlocks.
- The worker that completes the last Segment signals `job.done_cv`, and the Coordinator wakes.
- The Coordinator calls `MsgReplyv([reply header | Encrypted Block])`, and the kernel copies the data into the client's buffers.
- The client's sender thread unblocks and signals the main thread's condition variable.
- The client verifies the result against the Reference Encryption and by decrypting it.
- The server frees the Job and releases its slot. The workers stay alive for the next Job.

---

## 6. Verification

| What | How |
|---|---|
| Cipher correct | NIST SP 800-38A F.5.5 vectors, run at server start-up and in `make test`. The OpenSSL and bundled backends must produce byte-identical output. |
| Parallel = serial | Every Job in the tests is compared with the Reference Encryption, across 1–8 workers and sizes from 1 B to 2 MB. |
| No data races | `make tsan` runs every test under ThreadSanitizer. |
| Server logic | `test_e2e` runs the real `server.c` and `client.c` together on an in-process message-passing stand-in. It covers 16 clients at once, the Job cap, and every error reply. |
| On the Pi | `encclient` always verifies run 1, and with `-V` it verifies every run. |

---

## 7. Benchmark

- **Encryption Time**: measured by the server, from the first Task queued to the last Task finished. It is returned in the reply.
- **Round-Trip Time**: measured by the client, from `MsgSendv` to the reply. It includes both message copies.

`scripts/bench.sh` runs 1, 2, 4 and 8 workers, with 5 warm-up runs and 200 measured runs each, and writes `results.csv`. On the Pi 4B, software AES runs at roughly 50–100 MB/s per core. So a 2 MB Data Block takes tens of milliseconds on one worker, much longer than the threading overhead, and the speedup up to 4 workers should be clear. The 8-worker run shows what happens with more workers than cores.

To benchmark larger Data Blocks later, raise `ENC_MAX_BLOCK` in `src/core/config.h` and rebuild.

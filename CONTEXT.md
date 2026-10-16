# Concurrent Encryption Service

A QNX server that receives a Data Block from a client, encrypts its Segments in parallel, and replies with the Encrypted Block. It exists to demonstrate safe, measurable concurrency on QNX.

How the terms below relate to each other (colours follow the sections: grey Data, green Processing, amber Notification, blue Verification and Measurement):

```mermaid
flowchart TB
    FILE["File<br/>(not a server term)"] -- "client cuts into" --> DB["Data Block<br/>at most 2 MB"]
    DB -- "server handles as one" --> JOB["Job<br/>holds 1 of 8 slots"]
    JOB -- "split into" --> SEG["Segments"]
    SEG -- "one Task each" --> TASK["Task"]
    TASK -- "carried out by" --> WP["Worker Pool"]
    WP -- "fills" --> EB["Encrypted Block<br/>same length as the Data Block"]
    JOB -. "when done, server delivers" .-> EVT["Event<br/>a pulse"]
    EVT -- "client then sends" --> FETCH["Fetch"]
    FETCH -- "returns" --> EB
    EB -- "client writes, with a header" --> EF["Encrypted File"]
    JOB -. "client went away" .-> AJ["Abandoned Job"]
    REF["Reference Encryption<br/>one thread, no Segments"] -. "must equal" .-> EB
    JOB -. "server measures" .-> ET["Encryption Time"]
    FETCH -. "client measures, from the Data Block on" .-> RTT["Round-Trip Time"]

    classDef data fill:#f1f5f9,stroke:#475569,stroke-width:2px,color:#0f172a
    classDef proc fill:#dcfce7,stroke:#16a34a,stroke-width:2px,color:#14532d
    classDef note fill:#fef3c7,stroke:#d97706,stroke-width:2px,color:#78350f
    classDef check fill:#dbeafe,stroke:#2563eb,stroke-width:2px,color:#1e3a8a
    classDef warn fill:#fee2e2,stroke:#dc2626,stroke-width:2px,color:#7f1d1d
    class FILE,DB,EB,EF,SEG data
    class JOB,TASK,WP proc
    class EVT,FETCH note
    class REF,ET,RTT check
    class AJ warn
```

## Language

### Data

**Data Block**:
The bytes a client submits for encryption in one request, at most 2 MB. The server never sees files: the client sends a file as a sequence of Data Blocks.
_Avoid_: File, payload, message

**Encrypted Block**:
The bytes the server returns for one Data Block, the same length as the Data Block.
_Avoid_: Ciphertext buffer, output, result

**Encrypted File**:
What the client writes after encrypting a file: a short readable header, then the Encrypted Blocks of all the file's Data Blocks. The client can turn it back into the original file.
_Avoid_: Ciphertext file, output file, .enc

**Segment**:
A contiguous, independently encryptable slice of a Data Block. A Data Block of 64 KB or less is a single Segment.
_Avoid_: Chunk, part, piece

### Processing

**Job**:
The server's handling of one Data Block, from the moment it accepts the Data Block until the client fetches the Encrypted Block (or the Job is discarded). A Job holds one of a fixed number of slots for that whole time, so it also covers the wait between the Event and the Fetch. Several Jobs may be in progress at once.
_Avoid_: Request (that is the message, not the work), session, task

**Task**:
The unit of work that encrypts exactly one Segment of one Job.
_Avoid_: Job (a Job has many Tasks), work item

**Worker Pool**:
The fixed set of server threads that carry out Tasks from all Jobs.
_Avoid_: Thread pool (fine in code, but use Worker Pool when talking about the design)

**Abandoned Job**:
A Job whose client went away (it disconnected or died) before fetching the Encrypted Block. If its Tasks are still running, they finish and the Encrypted Block is discarded. If it had already finished, the server frees it at once. Either way its Job slot is released.
_Avoid_: Cancelled job (nothing is cancelled)

### Notification

**Event**:
The pulse that the client registers with the server and that the server delivers when a Job is done. It arrives on a private channel of the client's sender thread, and tells that thread to send the Fetch.
_Avoid_: Signal (a pulse is not a POSIX signal), callback, reply (the Event carries no data)

**Fetch**:
The second message for a Data Block, sent by the client after the Event. It names the Job, and the server answers it with the Encrypted Block. A Job can be fetched once, and only on the connection that sent the Data Block.
_Avoid_: Download, get result, poll

### Verification

**Reference Encryption**:
Encryption of a whole Data Block on a single thread, with no Segments, used to check that the Worker Pool produces identical bytes.
_Avoid_: Baseline, serial version

### Measurement

**Encryption Time**:
Server-side time for one Job from its first Task being queued to its last Task finishing.
_Avoid_: Processing time, server time

**Round-Trip Time**:
Client-side time from sending a Data Block to receiving its Encrypted Block. It spans the whole exchange: the Data Block message, the Event, and the Fetch.
_Avoid_: Latency, total time, end-to-end time

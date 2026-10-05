# Concurrent Encryption Service

A QNX server that receives a Data Block from a client, encrypts its Segments in parallel, and replies with the Encrypted Block. It exists to demonstrate safe, measurable concurrency on QNX.

## Language

### Data

**Data Block**:
The bytes a client submits for encryption in one request, at most 2 MB. The client may read it from a file, but the server never sees files.
_Avoid_: File, payload, message

**Encrypted Block**:
The bytes the server returns for one Data Block, the same length as the Data Block.
_Avoid_: Ciphertext buffer, output, result

**Segment**:
A contiguous, independently encryptable slice of a Data Block. A Data Block of 64 KB or less is a single Segment.
_Avoid_: Chunk, part, piece

### Processing

**Job**:
The server's handling of one Data Block, from receipt until the Encrypted Block is returned or discarded. Several Jobs may be in progress at once.
_Avoid_: Request (that is the message, not the work), session, task

**Task**:
The unit of work that encrypts exactly one Segment of one Job.
_Avoid_: Job (a Job has many Tasks), work item

**Worker Pool**:
The fixed set of server threads that carry out Tasks from all Jobs.
_Avoid_: Thread pool (fine in code, but use Worker Pool when talking about the design)

**Abandoned Job**:
A Job whose client went away before the reply. Its Tasks still finish, and its Encrypted Block is discarded.
_Avoid_: Cancelled job (nothing is cancelled)

### Verification

**Reference Encryption**:
Encryption of a whole Data Block on a single thread, with no Segments, used to check that the Worker Pool produces identical bytes.
_Avoid_: Baseline, serial version

### Measurement

**Encryption Time**:
Server-side time for one Job from its first Task being queued to its last Task finishing.
_Avoid_: Processing time, server time

**Round-Trip Time**:
Client-side time from sending a Data Block to receiving its Encrypted Block.
_Avoid_: Latency, total time, end-to-end time

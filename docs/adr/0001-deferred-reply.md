# Return the Encrypted Block through a deferred reply

The brief asks for a single request-response cycle and an asynchronous return. So the server keeps the client's `rcvid` and replies only when the Job is complete, and the Job Coordinator thread sends that reply (`MsgReplyv`), not the Receive Thread. The Receive Thread never waits for a Job, so the server keeps accepting new requests. The client does the blocking `MsgSendv` on its own sender thread, and its main thread gets a condition-variable signal when the reply arrives.

## Considered Options

- **Pulse notification, then a second request.** The server replies at once with a job ID, sends the client a pulse with `MsgDeliverEvent` when the Job is done, and the client then sends `GET_RESULT`. This way no client thread ever blocks. It was rejected because it needs two exchanges per Data Block, a result store with an expiry policy on the server, and a second message type, all against the single-cycle brief.
- **Reply from the Receive Thread after the Job completes.** This is the simplest option, but the server can then handle only one Job at a time, which defeats the "several Jobs at once" requirement.

## Consequences

- A client that dies mid-Job leaves an **Abandoned Job**. Its Tasks still run, and `MsgReplyv` then fails with `ESRCH`, so the Encrypted Block is discarded. Nothing is cancelled.
- Because the reply is deferred, the server must limit how many Jobs are open at once (`ENC_MAX_JOBS`). Otherwise memory use would grow with the number of waiting clients.

# Notify the client with an event, then let it fetch the Encrypted Block

Supersedes [ADR 0001](0001-deferred-reply.md).

The course requires that the server answers asynchronously **through an event**, and that the client gives up after 10 s. A deferred `MsgReplyv`, as in ADR 0001, is asynchronous but is not an event. So each Data Block now takes two exchanges and one event:

1. **ENCRYPT.** The client sends the Data Block together with a pulse event it has registered with `MsgRegisterEvent()`. The server copies the data with `MsgRead`, starts the Job, and replies at once with a Job id.
2. **Event.** When the Job is done, the Job Coordinator calls `MsgDeliverEvent()` with the client's event. The pulse arrives on a private channel owned by the client's sender thread, which waits for it with `MsgReceivePulse()`.
3. **FETCH.** The client sends the Job id, and the server replies with `MsgReplyv([result header | Encrypted Block])`.

Every blocking call on the client is preceded by `TimerTimeout()`, with one deadline of 10 s for the whole exchange. The client reaches the server only through `name_open()`. Its one `ConnectAttach()` is to its **own** channel, because a pulse needs a connection to travel on.

## Considered Options

- **Keep the deferred reply (ADR 0001).** It needs one exchange instead of two, but it does not use an event, which the brief now requires.
- **A signal event instead of a pulse** (`SIGEV_SIGNAL_CODE` with a real-time signal and `sigtimedwait()`). It needs no `ConnectAttach()`. It was rejected because signals are process-wide, so with several sender threads the right thread would have to be picked out by hand. Pulses are queued per channel and are the usual QNX way.
- **One channel per client process with a dispatcher thread.** It needs fewer channels, but adds a thread and a lookup from pulse to request. One channel per sender thread is simpler: each thread waits only for its own pulse.

## Consequences

- The server now **stores results** between the event and FETCH. A Job holds one of `ENC_MAX_JOBS` slots from ENCRYPT until FETCH. Further ENCRYPT requests wait in a FIFO (at most `ENC_MAX_WAITING`, then `EBUSY`), with their clients reply-blocked. The Receive Thread itself never stops receiving, otherwise it could not receive the FETCH that frees a slot.
- **Clients that go away** are handled with the channel's disconnect pulse: the server drops their waiting requests and finished Jobs, and marks running Jobs as Abandoned, so that their Coordinators free them when they finish.
- **Timeouts reach the server.** The `name_attach()` channel has `_NTO_CHF_UNBLOCK`, so when a client's timeout expires while its ENCRYPT waits for a slot, the server receives `_PULSE_CODE_UNBLOCK` and answers that request with `ETIMEDOUT`.
- The Round-Trip Time now includes the event and the FETCH exchange, so it is a little longer than before. The Encryption Time is unchanged.

# Game Runtime (GDK)

The Xbox Game Runtime APIs: `runtime/gdk` implements them against the public GDK's surface, so
a guest built against the GDK gets the types, layouts, and error codes it expects. Status per
function is in [hle-status.md](hle-status.md), "runtime/gdk".

**Status: XTaskQueue and XAsync implemented and tested (2026-11-23).** `test_gdk` checks both
on the host. The guest doesn't reach them yet: a title reaches the Game Runtime through
`xgameruntime.dll`'s `QueryApiImpl` and interface vtables ([hle-status.md](hle-status.md), the
notes for `runtime/gdk`), and that binding is the next step. The default policy never forwards
`xgameruntime.dll` to the host's copy, which talks to Xbox services (hard rule 5); `test_imports`
checks that.

## Why our own

A PC with the Gaming Services installed has an `xgameruntime.dll` of its own. We don't forward
to it, for three reasons:

* It reaches Xbox services for users, the store, and saves, which hard rule 5 forbids.
* Not every player has it, and its version follows the Store, not the game.
* XTaskQueue and XAsync are the base the other services run on. Their rules decide which thread
  runs a title's callbacks and when its memory may be freed, so a guest has to see one
  implementation we can test and reason about.

## Sources

The behavior follows libHttpClient's open-source task queue and XAsync implementation (MIT),
which the GDK's own documentation points to as the reference for these APIs. We read it for
behavior and wrote our own structure; no code is taken from it. Where it disagrees with the
documentation, we follow libHttpClient, since the console's Game Runtime is built from the same
design. Conformance rows reported by xbox-emulator and xcl, both run against the PC Game Runtime
or under Wine, agree with it ([related-work.md](related-work.md)).

Error codes come from the public `winerror.h` and `XGameErr.h`: `E_GAMERUNTIME_INVALID_HANDLE`
(0x8924010C) for a bad queue handle, `E_NO_TASK_QUEUE` (0x800701AB, `ERROR_NO_TASK_QUEUE`) when
an async call has no queue to use, and `ERROR_NOT_SUPPORTED` (0x80070032) for a result that
can't be fetched.

## XTaskQueue

A task queue runs callbacks somewhere other than where they were submitted. Each queue has two
**ports**, Work and Completion, and each port has a **dispatch mode** that decides where its
callbacks run:

| Mode | Where callbacks run |
|---|---|
| Manual | On the thread that calls `XTaskQueueDispatch` for that port |
| ThreadPool | On host thread-pool threads, several at once |
| SerializedThreadPool | On host thread-pool threads, one at a time, in submit order |
| Immediate | On the submitting thread, before the submit returns |

**Ports and composite queues.** A port is an engine of its own: the callbacks that are ready,
the ones waiting for their time, the wait handles it watches, and terminations waiting for it to
go idle. `XTaskQueueCreateComposite` makes a queue out of other queues' ports. Its callbacks run
where those ports run theirs, but it keeps its own termination state, so terminating a composite
queue cancels only what was submitted through it.

**Submitting.** `XTaskQueueSubmitCallback` queues a callback, and
`XTaskQueueSubmitDelayedCallback` queues it once the delay has passed. Delayed callbacks run in
the order they come due, through one host timer per port. `XTaskQueueRegisterWaiter` queues a
callback each time a wait handle is signaled. The queue keeps its own duplicate of the handle,
and it watches for the next signal only after the last one's callback has returned.

**Dispatching.** `XTaskQueueDispatch` runs at most one callback from the port on the calling
thread, waiting up to the timeout for one. It works on any port, not only Manual ones. A thread
that dispatches also runs the port's pending terminations once nothing else is waiting.
`XTaskQueueIsEmpty`, private to the Game Runtime, reports whether the port has nothing ready,
nothing delayed, and nothing running. A port's state is shared with every queue on it.

**Monitors.** A monitor is called on the submitting thread whenever a callback is queued on a
port its queue uses, before the callback can run. A title uses it to wake its own dispatch loop.
Once `XTaskQueueUnregisterMonitor` returns, the monitor isn't called again. The exception is a
monitor that unregisters itself from inside its own call, which can't wait for itself.

**Termination.** `XTaskQueueTerminate` ends a queue in a fixed order:

1. From the call on, submits, delayed submits, waiter registrations, and
   `XTaskQueueSuspendTermination` fail with `E_ABORT`.
2. Callbacks still waiting run with `canceled` set: queued ones in order, then delayed ones in
   the order they were due, then each registered waiter once more.
3. Once the work port has nothing of this queue's left, the completion port goes through the
   same steps.
4. The termination callback runs on the completion port.

With `wait`, the call returns only after the termination callback has. A Manual port has to be
dispatched for termination to get through it. `XTaskQueueSuspendTermination` holds step 3 or 4
off while work the queue doesn't track is outstanding, until the matching
`XTaskQueueResumeTermination`. XAsync uses it for every call.

**Handles.** A queue handle is a counted reference: `XTaskQueueDuplicateHandle` adds one and
`XTaskQueueCloseHandle` drops one. Every queued callback also holds a reference, so a callback
may close the last handle to its own queue. Port handles belong to their queue and are never
closed. A handle that isn't a live queue gives `E_GAMERUNTIME_INVALID_HANDLE`.

**The process queue.** `XTaskQueueGetCurrentProcessTaskQueue` returns the process queue, which
XAsync uses for a block with no queue. By default it is a queue with two ThreadPool ports, made
on first use. A title can replace it, or set it to null so that there is none.

## XAsync

XAsync is the Game Runtime's model for an asynchronous call. The caller owns an `XAsyncBlock`
(56 bytes: queue, context, completion routine, and 32 bytes of internal state) for the life of
the call. A **provider**, the code that implements a particular async API, does the work through
steps that XAsync calls: Begin, DoWork, GetResult, Cancel, and Cleanup.

**Lifecycle.**

* `XAsyncBegin` attaches the call to the caller's block and calls the provider's Begin step.
  * It fails with `E_INVALIDARG` when the block is already in a call, with `E_NO_TASK_QUEUE`
    when the block has no queue and there is no process queue, and with `E_ABORT` when the
    queue is terminating.
  * When the Begin step itself fails, `XAsyncBegin` still returns `S_OK`, and the call ends
    with that failure.
* The provider hands work to the queue with `XAsyncSchedule`, which queues its DoWork step on
  the work port. One schedule per call at a time: a second before the first has run gives
  `E_UNEXPECTED`.
* DoWork either completes the call or returns `E_PENDING` and schedules again later. A DoWork
  that returns success without completing the call is a provider bug, and the call ends with
  `E_UNEXPECTED`.
* `XAsyncComplete` ends the call with a result and the size of the result payload. Only the
  first completion counts, and `E_PENDING` ends nothing. The caller's completion routine then
  runs on the completion port.
* `XAsyncRun` is the simple form: one work callback runs on the work port, and its return value
  becomes the call's result.

**Status and results.**

* `XAsyncGetStatus` returns `E_PENDING` while the call runs, then its result. With `wait`, it
  returns only once the call is over and the completion routine has returned. A call that ended
  with no payload or with a failure is over at once, so a later wait returns straight away.
* `XAsyncGetResultSize` gives the payload size, or 0 while pending, after a failure, or when
  there is no payload.
* `XAsyncGetResult` fetches the payload through the provider's GetResult step. Its checks run in
  this order:
  1. A failed call's status is returned as is.
  2. A payload already fetched gives `E_ILLEGAL_METHOD_CALL`.
  3. A call with no payload gives `S_OK` and 0 bytes, however often it is asked.
  4. An identity that isn't the provider's gives `E_INVALIDARG`.
  5. A payload of size 0 gives `ERROR_NOT_SUPPORTED`.
  6. A null buffer gives `E_INVALIDARG`.
  7. A buffer that is too small gives `E_NOT_SUFFICIENT_BUFFER`.

  Any of the last four ends the call, as a successful fetch does.

**Cancel.** `XAsyncCancel` only asks: it calls the provider's Cancel step, and a provider that
can't cancel runs to its end. When the queue terminates, a call whose work step is still queued
is canceled the same way and, unless the provider has completed it, ends with `E_ABORT`. The
completion routine runs for a canceled call as for any other.

**Cleanup.** The provider's Cleanup step runs exactly once, after the call has completed: at
the end of the work step in which it completed, or else when the last reference to the call
goes. It never runs inside the provider's own Cancel step. `XAsyncBeginAlloc`, private to the
Game Runtime, gives the provider a zeroed, aligned context that the call owns and frees.

**Queues.** Each call holds a reference on its queue and suspends the queue's termination until
the call is over. So terminating a queue cancels the calls on it, runs their completion routines,
and finishes only after the last one. A call begun on a terminating queue fails with `E_ABORT`.

**Where the call lives.** The caller's block points to the call's state while the call is
attached; the provider works on a copy of the block that the call owns. When the call ends, its
final status is written back into the caller's block, so the caller may free its block as soon
as the completion routine has run, while the provider's steps still hold the copy.

## Tests

`test_gdk` (nogame) covers:

* each dispatch mode, delayed order, the serialized limit of one callback at a time
* termination order, canceled flags, `E_ABORT` after termination, and waiting for it
* suspended termination, monitors through a composite queue, and monitor removal
* waiters per signal and their last canceled call, handle checks, and the process queue
* XAsync through `XAsyncRun` and through a provider, payload fetches and their checks
* failing and immediate begin steps, scheduling rules, cancel, `XAsyncBeginAlloc`
* calls on a terminating queue

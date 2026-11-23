# Game Runtime (GDK)

The Xbox Game Runtime APIs: `runtime/gdk` implements them against the public GDK's surface, so
a guest built against the GDK gets the types, layouts, and error codes it expects. Status per
function is in [hle-status.md](hle-status.md), "runtime/gdk".

**Status: XTaskQueue, XAsync, XError, XThread, XUser, and XGameSave implemented and bound
(2026-11-23).** The
guest reaches them the way a title does on the console: through our `xgameruntime.dll`, its
`QueryApiImpl`, and the interface vtables ("The Game Runtime DLL" below). The default policy never
forwards `xgameruntime.dll` to the host's copy, which talks to Xbox services (hard rule 5);
`test_imports` checks that, and `test_gdk` calls every service through its slots.

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

## The Game Runtime DLL

A title doesn't import Game Runtime functions by name. The GDK's `xgameruntime.lib` is linked
into each module that uses the runtime, and its code loads `xgameruntime.dll` at run time and
asks it for one object per service. We checked this code in the console's libHttpClient and
XCurl. A GDK title links the same library into its executable.

**Start-up.** The first lookup starts the runtime, under a lock:

1. If `xgameruntime.dll` sits next to the executable, the library loads it with
   `LoadLibraryExW` and flags `0xA00` (application directory and System32). Otherwise it uses
   `0x800` (System32 only). The game's folder has no such file, so the load is by name with
   `0x800`, and our loader answers with a pseudo handle because the policy provides the DLL.
2. It looks up `QueryApiImpl`, `InitializeApiImpl`, `InitializeApiImplEx`,
   `InitializeApiImplEx2`, `XErrorReport`, and `UninitializeApiImpl`. It fails with 0x89240101
   unless `QueryApiImpl`, at least one initializer, `XErrorReport`, and `UninitializeApiImpl`
   are all there. A failed load gives the same code.
3. It calls the newest initializer there is:
   * `InitializeApiImplEx2` with two packed versions, a flags byte, and an options pointer.
   * `InitializeApiImplEx` with the same first three arguments.
   * `InitializeApiImpl` with the two 32-bit halves of the first version.

   In this build's libraries the versions read 25398.10002 and 26100.6194, which look like the
   console OS and Windows builds the GDK targets, and the library sets bit 1 of the flags. A
   negative result fails the lookup. Other errors go to `XErrorReport`, with 0x89240100 or
   0x8924010A.

**Lookups.** Each thunk passes a class ID and an interface ID to a wrapper, which starts the
runtime if needed and then calls `QueryApiImpl(class, interface, &object)`. The threading,
error, and feature thunks pass the same GUID twice; the XUser and XGameSave thunks pass their
class with one of two interfaces.
The thunk then calls one vtable slot of the object and releases it (slot 2). A wrapper that
gets `REGDB_E_CLASSNOTREG` (0x80040154) turns it into 0x89240107 and reports it through
`XErrorReport`.

**Ours.** `runtime/gdk/src/runtime.cpp` provides the six entry points, and
`default_game_policy()` binds them as HLE for `xgameruntime.dll`:

* `QueryApiImpl` returns static objects, so the reference counts never reach zero. It gives
  `E_POINTER` for a null argument. For a service we don't provide yet it gives
  `REGDB_E_CLASSNOTREG`, which the guest reports, and for an interface a service doesn't have,
  `E_NOINTERFACE`; it logs each missing class, and each missing interface of a class, once.
  `IUnknown` gives a class's first interface. An object's own slot 0 answers for the other
  interfaces of its class.
* The initializers log their arguments once and succeed. `UninitializeApiImpl` succeeds.
* Every other export is `E_NOTIMPL` for an import, and `GetProcAddress` finds nothing for it.
  `RestartApiImpl`, `RundownApiImpl`, and `GetApiVersionImpl` are such names: XCurl looks them
  up only in `gameplatformservices.dll`, its Gaming Services path, which stays missing offline.

**Slots.** Each service object is COM-style: slots 0 to 2 are `IUnknown`'s, and every later
slot takes the object first, then the public function's arguments. We checked each slot on the
console's libHttpClient (27 threading thunks) and XCurl (8): how many arguments the thunk
passes, and whether it uses the result and how wide it is.

| Slot | Threading (073b7dcb-1fcf-4030-94be-e3c9eb623428) |
|---|---|
| 3 to 7 | `XAsyncGetStatus`, `XAsyncGetResultSize`, `XAsyncCancel`, `XAsyncRun`, `XAsyncBegin` |
| 8 | Unknown: no thunk calls it. Fails with `E_NOTIMPL` |
| 9 to 11 | `XAsyncSchedule`, `XAsyncComplete`, `XAsyncGetResult` |
| 12 to 17 | `XTaskQueueCreate`, `XTaskQueueCreateComposite`, `XTaskQueueGetPort`, `XTaskQueueDuplicateHandle`, `XTaskQueueDispatch`, `XTaskQueueCloseHandle` |
| 18 to 22 | `XTaskQueueSubmitCallback`, `XTaskQueueSubmitDelayedCallback`, `XTaskQueueRegisterWaiter`, `XTaskQueueUnregisterWaiter`, `XTaskQueueTerminate` |
| 23 to 26 | `XTaskQueueRegisterMonitor`, `XTaskQueueUnregisterMonitor`, `XTaskQueueGetCurrentProcessTaskQueue`, `XTaskQueueSetCurrentProcessTaskQueue` |
| 27 | `XThreadSetTimeSensitive` |
| 28 | Unknown: the thunk passes no arguments and returns a 32-bit value. Fails with `E_NOTIMPL` |
| 29, 30 | `XThreadAssertNotTimeSensitive`, `XThreadIsTimeSensitive` |

| Slot | Error (8ca467f7-22e8-4096-8456-bb8aa13f79d8) |
|---|---|
| 3 | Unknown: two arguments, no result. Logged once and ignored |
| 4 | `XErrorSetCallback` |
| 5 | `XErrorSetOptions` |

| Slot | Feature (8836fe87-edb9-4fe3-8dad-05f0d2cd5b40) |
|---|---|
| 3 | `XGameRuntimeIsFeatureAvailable`: one argument, returns `bool` |

The XUser and XGameSave slots are in their own sections below.

The threading order matches xgameruntime-rs (MIT,
[related-work.md](related-work.md)), with one exception. xgameruntime-rs puts a private
`XTaskQueueIsEmpty` at slot 28, but the thunk there passes no queue and no port, so it can't be
that function. `XTaskQueueIsEmpty` and `XAsyncBeginAlloc` stay in `runtime/gdk` for our own use
until a slot is shown to reach them. Leonida reports that slot 8 fails on the PC runtime.

`XGameRuntimeIsFeatureAvailable` answers true for every feature, as the console does, so the
game takes the paths it took there. A service we don't provide yet then fails at `QueryApiImpl`,
where the log names it. Each feature number asked for is logged once.

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

## XError

`XErrorReport(result, message)` is how libraries report an error to the runtime, and
`XErrorSetCallback` and `XErrorSetOptions` let the title decide what happens next.

* Every report goes to our log first, whatever the title asked for.
* Then the title's callback runs, outside our lock, so it may report again or change the
  callback. A callback that returns false ends the handling there.
* Then the options for the current case (debugger attached or not) apply:
  `OutputDebugStringOnError` writes the message, and `DebugBreakOnError` breaks into an attached
  debugger. `FailFastOnError` is logged as an error but not acted on, so that during bring-up a
  reported error can't end the game before the log shows why.
* Both option sets start as `None`. `XErrorReport` returns `S_OK`.

Our own services don't report their failures through XError yet. The console's runtime does, so
a title that relies on its callback for diagnostics sees less here.

## XThread

A title marks the threads that must never block, such as its render thread, with
`XThreadSetTimeSensitive`, and `XThreadIsTimeSensitive` reads the mark back. The mark is per
thread. `XThreadAssertNotTimeSensitive` is what the runtime's blocking functions call: on a
marked thread it logs a warning, once per thread, and the call goes on. The guest reaches it
through one thunk, so a caller address would only name the thunk.

## XUser

XUser gives the game one local user, offline, and always signed in. Every request to add a user
returns it, without UI, whatever the options. Hard rule 5 decides the rest: nothing that needs
the Xbox network succeeds.

**Interfaces.** The console's libHttpClient reaches XUser through two classes, and we checked
every thunk for its slot, the number of arguments it passes, and whether it keeps the result:

* class 01acd177-91f9-4763-a38e-ccbb55ce32e0 with interface 26f3c674-a2fe-44fa-b6c4-a323bc94ff53
  (43 thunks), or with interface cef4fac0-7676-4a94-a119-4c43f9eb5b74 (1 thunk, the gamertag);
* class 7d824997-10dc-45ab-86b7-2737767c0bf1 with interface 0cc6a956-e7e1-4fdf-9341-9d5da649ebc8
  (8 thunks, devices).

| Slot | User (26f3c674) |
|---|---|
| 3 to 6 | `XUserDuplicateHandle`, `XUserCloseHandle` (no result), `XUserCompare` (32-bit), `XUserGetMaxUsers` |
| 7, 8 | `XUserAddAsync`, `XUserAddResult` |
| 9 to 14 | `XUserGetLocalId`, `XUserFindUserByLocalId`, `XUserGetId`, `XUserFindUserById`, `XUserGetIsGuest`, `XUserGetState` |
| 15 | No thunk. Fails with `E_NOTIMPL` |
| 16 to 18 | `XUserGetGamerPictureAsync`, `XUserGetGamerPictureResultSize`, `XUserGetGamerPictureResult` |
| 19 to 22 | `XUserGetAgeGroup`, `XUserCheckPrivilege`, `XUserResolvePrivilegeWithUiAsync`, `XUserResolvePrivilegeWithUiResult` |
| 23 to 28 | `XUserGetTokenAndSignatureAsync`, `ResultSize`, `Result`, then the same three in UTF-16 |
| 29 to 32 | `XUserResolveIssueWithUiAsync`, `XUserResolveIssueWithUiResult`, then both in UTF-16 |
| 33 to 36 | `XUserRegisterForChangeEvent`, `XUserUnregisterForChangeEvent` (`bool`), `XUserGetSignOutDeferral`, `XUserCloseSignOutDeferralHandle` (no result) |
| 37 to 41 | Not provided: their thunks pass 2, 2, 4, 4, and 2 arguments. Fail with `E_NOTIMPL` |
| 42 | `XUserIsStoreUser` (`bool`) |
| 43 to 46 | Not provided: 2, 1, 3, and 2 arguments. Fail with `E_NOTIMPL` |

| Slot | Gamertag (cef4fac0) |
|---|---|
| 3 | `XUserGetGamertag` |

| Slot | Devices (0cc6a956) |
|---|---|
| 3 to 5 | `XUserFindForDevice`, `XUserRegisterForDeviceAssociationChanged`, `XUserUnregisterForDeviceAssociationChanged` (`bool`) |
| 6 to 8 | `XUserGetDefaultAudioEndpointUtf16`, `XUserRegisterForDefaultAudioEndpointUtf16Changed`, `XUserUnregisterForDefaultAudioEndpointUtf16Changed` (`bool`) |
| 9, 10 | `XUserFindControllerForUserWithUiAsync`, `XUserFindControllerForUserWithUiResult` |

xgameruntime-rs (MIT) names the same functions at the same slots up to 36, and at 42. It puts
device association at 37 and 38, but the console's thunks there pass two arguments each, which
doesn't fit. Their argument counts fit `XUserAddByIdWithUiAsync` and its result, and
`XUserGetMsaTokenSilentlyAsync` with its result and result size, but nothing shows that yet.
xgameruntime-rs's PC platform functions (remote connect and sign-in prompt handlers) fit slots 43
to 46 by argument count. An offline user needs none of them, so they fail, logged once, until
the game is seen calling one.

**The user.** One handle, the same for every reference: `XUserDuplicateHandle` and every find
add a reference and `XUserCloseHandle` drops one. Any other handle fails with `E_INVALIDARG`.

* Local ID 1, and user ID 0x0009000000000001, in the range Xbox user IDs use.
* Signed in, not a guest, age group Adult, and `XUserGetMaxUsers` is 1.
* Gamertag "Player" for the classic, modern, and unique modern forms, and an empty suffix.
* `XUserFindUserByLocalId` and `XUserFindUserById` give `E_GAMEUSER_USER_NOT_FOUND`
  (0x89245104) for any other ID, as the console does.
* `XUserIsStoreUser` is true, since the player owns the game.

**Privileges.** Game DVR, cloud saved games (they map to local saves), and user-generated
content are granted. Every other privilege the public GDK lists is online (multiplayer, parties,
sessions, communications, cross-play, clubs, broadcast, friends, sharing) and is denied with
reason `Restricted`, which titles show as a setting rather than a purchase prompt. An unknown
privilege is denied and logged once. Resolving a privilege shows no UI: a granted one resolves
with `S_OK`, a denied one with `E_ABORT`, as a dismissed dialog does.

**Calls that can't succeed offline** fail through the queue, so the caller's completion routine
runs as it would on the console:

* Tokens and the gamer picture end with 0x80072EFD (`ERROR_INTERNET_CANNOT_CONNECT`), which xcl
  reports as the console's answer when it can't reach the Xbox network. Titles take their
  offline paths on it. The result functions return that status and set no data.
* `XUserResolveIssueWithUiAsync` and `XUserFindControllerForUserWithUiAsync` end with `E_ABORT`,
  as a dismissed dialog does.
* `XUserGetSignOutDeferral` gives `E_GAMEUSER_DEFERRAL_NOT_AVAILABLE` (0x89245103), because no
  sign-out ever comes.
* `XUserGetDefaultAudioEndpointUtf16` gives `ERROR_NOT_FOUND`: there is no chat endpoint.

**Callbacks.** Change, device association, and audio endpoint registrations get distinct tokens
and are kept until unregistered, which always succeeds. Nothing calls them yet, because the user
never changes. Every device belongs to the one user, so `XUserFindForDevice` returns it for any
ID. Once input exists (M4), a gamepad has to be reported moving from no user to this user
through the device association callback, on the title's queue, because some titles count a pad
as connected only then ([hle-status.md](hle-status.md)).

## XGameSave

XGameSave keeps the game's saves in the per-user data directory, in `<data>\saves`, which the
runner sets before the guest starts. Nothing syncs anywhere (hard rule 5): no container ever
needs a sync, and no call waits on a network. The behavior follows the public GDK
documentation. Where it is silent, the rows other projects report from the PC runtime decide
([related-work.md](related-work.md)). The rest is our choice, marked as such below.

**Interfaces.** One class, 704c3f58-e629-4cc2-b197-30511b996fe2, with two interfaces whose thunks
call one vtable: 1bfff3af-f14a-40a3-8e35-9ada906593f9 (30 thunks, slots 3 to 32) and
ab4ae4fb-6508-4950-a032-45fd4bf8c43b (17 thunks, slots 33 to 49). We checked every thunk in the
console's libHttpClient for its slot, the number of arguments it passes, and whether it keeps the
result. xgameruntime-rs (MIT) has the same order through slot 32.

| Slot | XGameSave (1bfff3af; ab4ae4fb from 33) |
|---|---|
| 3 to 6 | `XGameSaveInitializeProvider`, `InitializeProviderAsync`, `InitializeProviderResult`, `CloseProvider` (no result) |
| 7 to 9 | `XGameSaveGetRemainingQuota`, `GetRemainingQuotaAsync`, `GetRemainingQuotaResult` |
| 10 to 12 | `XGameSaveDeleteContainer`, `DeleteContainerAsync`, `DeleteContainerResult` |
| 13 to 17 | `XGameSaveGetContainerInfo`, `EnumerateContainerInfo`, `EnumerateContainerInfoByName`, `CreateContainer`, `CloseContainer` (no result) |
| 18, 19 | `XGameSaveEnumerateBlobInfo`, `EnumerateBlobInfoByName` |
| 20 to 22 | `XGameSaveReadBlobData`, `ReadBlobDataAsync`, `ReadBlobDataResult` |
| 23 to 29 | `XGameSaveCreateUpdate`, `CloseUpdate` (no result), `SubmitBlobWrite`, `SubmitBlobDelete`, `SubmitUpdate`, `SubmitUpdateAsync`, `SubmitUpdateResult` |
| 30 to 32 | `XGameSaveFilesGetFolderWithUiAsync`, `XGameSaveFilesGetFolderWithUiResult`, `XGameSaveFilesGetRemainingQuota`: not provided, `E_NOTIMPL`, logged once |
| 33 to 49 | Unknown: 2, 1 (no result), 2, 3, 2, 1, 4, 2, 2, 2, 2, 2, 3, 1, 3, 1, and 2 arguments. Fail with `E_NOTIMPL` |

xgameruntime-rs names slots 33 to 49 only in comments (a configuration initializer and UI
callbacks among them), and nothing here confirms those names. They stay unknown, each logged
once, until the game is seen calling one.

**Providers.** Only the offline user's handle opens one; any other gives `E_INVALIDARG`. A null
or empty configuration ID gives `E_GS_NO_SERVICE_CONFIGURATION` (0x80830009). Each
configuration ID has its own folder, so two configurations never see each other's containers.
Closing a provider expires its containers and updates: later calls on them give
`E_GS_HANDLE_EXPIRED` (0x8083000D), as a closed handle does. A null handle gives
`E_INVALIDARG`. Until the runner sets the save directory, providers fail with `E_GS_NO_ACCESS`
(0x80830002), logged.

**Containers.** Names follow the public rules: 1 to 256 characters. Up to the last `/`, only
letters, digits, `_`, and `/` are allowed. After it comes a non-empty name of letters, digits,
`_`, `.`, and `-` that doesn't end in `.` or contain `..`. Anything else gives
`E_GS_INVALID_CONTAINER_NAME` (0x80830001), in delete too, where xcl found that an empty name
once deleted a whole save folder. Names match without case, and a container keeps the spelling
it was first saved with.

* `XGameSaveCreateContainer` opens a container whether or not it exists. A container exists
  once an update to it is submitted, so an empty update makes an empty container.
* Enumerations list containers and blobs in name order, compared without case. The `ByName`
  forms take a prefix, compared the same way. A callback that returns false ends the
  enumeration.
* `last_modified_time` is the time of the last update, and `needs_sync` is always false.
* Our choices, to check against the game: `XGameSaveGetContainerInfo` on a container that
  doesn't exist fails with `HRESULT_FROM_WIN32(ERROR_NOT_FOUND)` (0x80070490) without calling the
  callback, and deleting one succeeds.

**Updates.** An update collects blob writes and deletes, copying the data as each write is
submitted to it. Submitting the update applies every change, or none:

* New blobs go to new files, flushed to the disk, and then one rename replaces the container's
  manifest. A crash leaves the old manifest whole, and the files it left are never listed and
  are removed by the next update.
* A delete moves the container's folder aside in one rename before removing it. A crash leaves
  no half-deleted container, and the next provider removes what is left.
* The last change to a blob name wins, and a null display name keeps the container's.
* One update writes at most 16 MB (`GS_MAX_BLOB_SIZE`), in one blob or all together; more
  gives `E_GS_UPDATE_TOO_BIG` (0x80830005).
* An update is submitted once: a second submit, or a change after the first, gives
  `E_GS_HANDLE_EXPIRED`.
* A submit that would take a configuration past its 256 MB quota gives `E_GS_QUOTA_EXCEEDED`
  (0x80830006) and changes nothing. `XGameSaveGetRemainingQuota` gives the quota less every
  blob's size. A full disk gives `E_GS_OUT_OF_LOCAL_STORAGE` (0x80830003).

**Reads.** `XGameSaveReadBlobData` fills the caller's buffer with one 24-byte `XGameSaveBlob`
per blob, then each blob's data in the same order, without padding, as xbox-emulator reports.
Null names read every blob, in name order. The names point to storage that lasts as long as the
process. A missing blob gives `E_GS_BLOB_NOT_FOUND` (0x80830008), and a buffer too small for
everything gives `E_GS_PROVIDED_BUFFER_TOO_SMALL` (0x80830007). The asynchronous form's result
size covers the records and the data, and `XGameSaveReadBlobDataResult` counts the records from
where the first blob's data starts. A buffer smaller than that size gets `XAsyncGetResult`'s
`E_NOT_SUFFICIENT_BUFFER`.

**Threads.** The synchronous calls that touch the disk (initialize, quota, delete, read, and
submit) fail with `E_GS_ASYNC_FUNCTION_REQUIRED` (0x8083000E) on a thread marked time-sensitive,
logged once each, and leave an update as it was. The asynchronous forms do their work on the
queue's work port, in queue order. On a queue that is shutting down they give `E_ABORT` and do
nothing, and an update refused that way can still be submitted. Disk work runs under one lock,
so updates and deletes apply one at a time.

**On disk.** `<data>\saves\s-<configuration>\c-<container>\` holds `manifest.txt` and one
numbered `.blob` file per blob.

* A folder name is the name in lower case, with every character outside `a-z0-9_.-` and a
  trailing dot escaped as `%xx`. The prefix keeps it clear of `.`, `..`, and device names such as
  `CON`. A name longer than 100 characters keeps its first 80, then `~` and a 64-bit hash of the
  whole name.
* The manifest is a small text file with the container's real name, its display name, its time,
  and each blob's name, size, and file. A damaged one is logged and its container left alone;
  calls on it fail with `ERROR_FILE_CORRUPT` (0x80070570).

## Tests

`test_gdk` (nogame) covers:

* each dispatch mode, delayed order, the serialized limit of one callback at a time
* termination order, canceled flags, `E_ABORT` after termination, and waiting for it
* suspended termination, monitors through a composite queue, and monitor removal
* waiters per signal and their last canceled call, handle checks, and the process queue
* XAsync through `XAsyncRun` and through a provider, payload fetches and their checks
* failing and immediate begin steps, scheduling rules, cancel, `XAsyncBeginAlloc`
* calls on a terminating queue
* `QueryApiImpl`'s answers, the services' `IUnknown` slots, and each service called through its
  slots as a thunk calls it: queues and async calls, the time-sensitive mark per thread, the
  unknown slots, XError's callback, and the feature query
* XUser: adding the user through the queue, its handle and identity, finds for other IDs, the
  gamertag and its buffer checks, the privilege policy and resolving it, the calls that fail
  offline or without UI, registrations, devices, and its three interfaces called through their
  slots, including `IUnknown` across a class
* XGameSave: providers for the offline user and their configurations, containers that appear
  on submit, updates that write, replace, and delete together, reads into one buffer directly
  and through the queue, the name rules and case, prefixes, deletes in queue order and on a
  terminating queue, the update and quota limits, saves across providers, files a crash leaves
  behind, expired handles, the time-sensitive rule, and the slots through `QueryApiImpl`
* the initializers

`test_imports` checks that the default policy binds the six entry points to these functions and
nothing else in `xgameruntime.dll`.

# Debugger threading model

Which thread may touch what in the WebSocket debugger (headless builds; the libretro core doesn't
build it). Read this before touching `Core/Debugger/WebSocket*`, `Core/WebServer.cpp` or anything
else that reaches into core state from outside the CPU thread.

There are no locks here, and none should come back. State has one owner; other threads hand it
work or messages.

## Threads

- **CPU thread** - runs `Core_RunLoopUntil()` and owns all emulator state, including the debugger's
  (`g_breakpoints`, `g_symbolMap`, `g_disassemblyManager`, `MIPSAnalyst`'s function tables, the
  `MemBlockInfo` slab maps, registers, memory, kernel objects).
- **Web server thread** (`Core/WebServer.cpp`) - blocks in `http::Server::RunSlice()` on the
  listening socket and the server's `net::CancelToken`, with no timeout. `ShutdownWebServer()`
  cancels the token, which wakes the accept loop and every connection, then joins the thread.
- **Connection threads** - one per request (`NewThreadExecutor`). A debugger session runs
  `HandleDebuggerRequest()` there. The server thread joins them all before it exits.

## Rules

- **Emulator state is accessed only inside `Core_RunOnCPUThread(func)`** (`Core/Core.h`). It queues
  `func` for the CPU thread and blocks until it has run (inline if already on the CPU thread). Put
  the validity checks (`isAlive()`, `IsValidAddress()`) inside the callback too; answering them
  outside means acting on a stale answer.
- **Never make the CPU thread wait for a thread that may be waiting on the CPU thread.** A
  `Core_RunOnCPUThread()` callback must not block on a connection. The queue is drained only while
  the CPU loop runs, so don't rely on it where the loop may never run again (shutdown paths).
- **Events the emulator produces on its own are pushed, never polled.** Each connection has a
  `DebuggerEventSink` (a `DebuggerMailbox`, see `WebSocketUtils.h`): an MPSC inbox of formatted
  events plus a wake socket. `WebSocketDebuggerTick()`, called from `Core_ProcessCPUQueue()` on the
  CPU thread, notices transitions (`game.*`, `cpu.stepping`, `input.buttons`/`input.analog`,
  finished `input.buttons.press`) and posts them; breakpoint hits are posted from
  `WebSocketNotifyBreakpointHit()`. Don't add a broadcaster that reads emulator state from the
  connection thread.
- **A connection blocks until something happens**: socket readable/writable, its sink woken, or the
  server stopping (`WebSocketServer::Process(wakeFds, count)`, no timeout). Producers publish first,
  then call `mailbox->Wake()`; wakes are coalesced with a pending flag, and the connection rearms
  (drains the socket, then clears the flag) before looking at its queues.
- Subscribers fed from another thread (`GPUStatsSubscriber`'s flip listener, `GPURecordSubscriber`'s
  record callback) push into their own MPSC queue and call `mailbox->Wake()`; their `Broadcast()`
  drains it on the connection thread. Log messages arrive the same way (`LogBroadcaster`).
- Sinks are refcounted: the connection and the CPU thread's list each hold one reference, so a
  callback on the CPU thread can still post after the connection has left.

## Memory tagging

`NotifyMemInfo*()` may be called from any thread (CPU thread, software rasterizer workers, the SAS
thread). Notifications go into an MPSC queue; the slab maps belong to the emulation thread, which
drains the queue before every lookup (`FindMemInfo*`, `FormatMemWriteTagAt`), on every flip, and for
savestates. Lookups are emulation-thread only.

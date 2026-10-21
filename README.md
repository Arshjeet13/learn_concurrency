# Toy A: Fork/Respawn Supervisor

A parent process that keeps exactly N worker processes alive. If any worker dies, for any reason, the parent notices, reports how it died, cleans it up, and starts a replacement. The parent never blocks waiting on any one child.

This is the skeleton of every device in the cluster. In V0 the parent will also run the epoll intake loop, and the workers will run inference. This toy builds only the "keep workers alive" part, so you can test it in isolation.

---

## 1. Why this matters for the cluster

| Cluster need | What this toy teaches |
|---|---|
| Workers are processes, not threads, so a crash is isolated | `fork`, separate address spaces |
| The parent detects a dead worker and respawns it | `waitpid`, exit status decoding |
| Detection must not block the network intake loop | `signalfd` inside an epoll loop |
| Requeue the dead worker's request | A slot table mapping PID to worker slot (the ancestor of the in-progress marker) |
| A debug trigger that crashes a worker on command | Stretch goal S3 |

---

## 2. Concepts you need (and the terms used below)

**Process lifecycle.** `fork()` creates a child that is a copy of the parent. It returns twice: 0 in the child, and the child's PID in the parent. The child runs until it calls `exit`, returns from `main`, or is killed by a signal.

**Zombie.** When a child exits, the kernel frees its memory but keeps a small record (PID and exit status) so the parent can find out what happened. Until the parent collects that record, the child is a zombie and shows as `Z` / `<defunct>` in `ps`. Zombies use a PID slot each, and enough of them will stop the system from creating new processes.

**Reaping.** Collecting a zombie's exit status with `wait`/`waitpid`. After that, the record is gone.

**Orphan.** A child whose parent died first. It gets reparented to PID 1 (or to a "subreaper" process), which reaps it eventually. Orphans are not zombies.

**Signal.** An asynchronous notification the kernel delivers to a process, such as SIGCHLD ("one of your children changed state"), SIGKILL (from `kill -9`), or SIGSEGV (bad memory access). Each signal has a *disposition*: default action, ignore, or a handler function.

**Signal mask.** The set of signals a process has *blocked*. A blocked signal is not lost. It stays *pending* until it is unblocked or read some other way. You change the mask with `sigprocmask`. A forked child inherits the parent's mask.

**Standard signals don't queue.** If SIGCHLD is already pending and another child dies, you still have one pending SIGCHLD, not two. Keep this in mind when you design your reaping loop.

**signalfd.** A Linux-specific syscall that creates a file descriptor you `read()` signals from, instead of having them delivered to a handler. Since it is an fd, you can add it to epoll, and "a child died" becomes an ordinary event in your loop. This avoids signal handlers, which can interrupt your code at any instruction and may only call a short list of async-signal-safe functions.

**`waitpid(pid, &status, options)`.**
- `pid = -1` means any child.
- `WNOHANG` means return 0 right away if no child has exited, instead of blocking.
- The return value is the reaped PID, 0 (nothing to reap yet, with WNOHANG), or -1 with `errno == ECHILD` (no children at all).
- Decode `status` with the macros `WIFEXITED`/`WEXITSTATUS` (normal exit and its code) and `WIFSIGNALED`/`WTERMSIG` (killed by which signal). `WCOREDUMP` is a common extension.

**epoll recap.** You already used this in echo3. Here the only fd in the interest list is the signalfd, at least for now.

---

## 3. Build it in two steps

### Step 1: blocking supervisor (no epoll)

Spec:
1. The parent forks `N = 4` workers.
2. Each worker prints `worker <slot> pid=<pid> started`, then loops forever: sleep 1 second, print a heartbeat line.
3. The parent loops on a blocking `waitpid(-1, &status, 0)`. For each reaped child, it prints:
   `slot <k> pid=<pid> died: exited code=<c>` or `... died: killed by signal <s> (<name>)`.
   Then it forks a replacement into the **same slot**.
4. The parent keeps a slot table: an array of N entries, each holding the PID currently in that slot. When a PID is reaped, look up its slot. The replacement goes there.

Done when: `kill -9` on any worker produces a correct death message and a new worker in the same slot, and `ps` shows no zombies.

### Step 2: event-driven supervisor (signalfd + epoll)

Spec:
1. Same workers and the same slot table.
2. The parent blocks SIGCHLD, creates a signalfd for it, creates an epoll instance, and registers the signalfd.
3. Main loop: `epoll_wait` with no timeout. When the signalfd is readable:
   - `read` the `struct signalfd_siginfo` out of it to consume the notification.
   - Reap **every** child that is ready, not just one (see the "don't queue" note above). Stop when `waitpid` says there is nothing left to reap right now.
   - Respawn each reaped slot.
4. Add SIGINT and SIGTERM to the same signalfd. On either one, the parent does a **graceful shutdown**: sends SIGTERM to every worker, reaps all of them, prints a summary (how many respawns per slot), and exits 0.

Done when: every test in section 4 passes, and Ctrl-C leaves no workers running (`pgrep -f <your binary>` is empty).

Why the shutdown step matters: in V0 this is how you stop a device cleanly between benchmark runs, instead of leaving orphaned workers that skew the next run.

---

## 4. Test plan

Open two terminals. Run the supervisor in one. In the other:

| # | Test | Command idea | Expected |
|---|---|---|---|
| T1 | Kill one worker | `kill -9 <pid>` | One death line with signal 9 (SIGKILL), one respawn in the same slot |
| T2 | Kill two at once | `kill -9 <pid1> <pid2>` | **Two** death lines and two respawns, even if only one SIGCHLD was read |
| T3 | Kill all workers | `pkill -9 -P <parent_pid>` | N deaths, N respawns |
| T4 | Normal exit | Make worker 0 call `exit(3)` after 5 seconds | `exited code=3`, respawned |
| T5 | Crash | Make worker 1 dereference a null pointer after 7 seconds | `killed by signal 11 (SIGSEGV)` |
| T6 | No zombies | `ps -o pid,ppid,stat,cmd --ppid <parent_pid>` after T1-T5 | No `Z` in the STAT column |
| T7 | Graceful shutdown | Ctrl-C the parent | Summary printed, `pgrep` finds no workers |
| T8 | Parent killed hard | `kill -9 <parent_pid>` | Workers are orphaned and keep running. Note what their PPID becomes. (This is not a bug to fix here. See S4.) |

T2 is the important one. If it fails only sometimes, the bug is in your reaping loop.

---

## 5. Pitfalls (read these before debugging)

- **Duplicated output after fork.** `std::cout` (and C's `stdout`) is buffered in user space. If the parent has unflushed output when it forks, the child gets a copy of that buffer and prints it too. This is most visible when you redirect output to a file. Think about when to flush.
- **How a C++ worker should exit.** `exit()` (or returning from `main`) in a forked child runs static destructors and flushes inherited buffers that belong to the parent. Look up `_exit()` and decide which one a worker should use.
- **Explicitly ignoring SIGCHLD changes `wait`.** Setting SIGCHLD's disposition to `SIG_IGN` makes the kernel auto-reap children, and `waitpid` then fails with `ECHILD`. Blocking a signal and ignoring it are different things.
- **The child inherits more than memory.** It also inherits the signal mask and every open fd, including the epoll fd and the signalfd. Decide what each worker should do with these.
- **`ssi_pid` is not a full list.** The `signalfd_siginfo` you read names one child. It doesn't tell you how many died. Let `waitpid` tell you.
- **Check every return value.** `fork` can fail (`EAGAIN` when you hit the process limit). `read` on the signalfd can return a short count. Print `strerror(errno)` on failure. Silent failures were the main problem in echo2.

---

## 6. Questions to answer in NOTES.md before step 2

1. Why must SIGCHLD be blocked *before* the signalfd is created? What happens to a SIGCHLD that arrives while it is unblocked with its default disposition?
2. If two children die at nearly the same moment, how many SIGCHLD notifications can you receive? What does that mean for how many times you call `waitpid` per epoll event?
3. After `fork`, the worker holds copies of the epoll fd and signalfd. Should it close them? What could go wrong if it doesn't?
4. (CS537 tie-in) Right after `fork`, parent and child have identical memory contents. Are those physically the same pages? What happens the first time the child writes to a variable? (This sets up Toy B, where you deliberately make memory *shared*.)

---

## 7. Stretch goals (optional, each is small)

- **S1. Restart limit.** A worker that crashes right on startup causes an infinite respawn loop that burns a CPU core. If a slot dies more than K times in T seconds, stop respawning it and log the slot as failed. (Real supervisors such as systemd do this.)
- **S2. Uptime per slot.** Record the start time of each worker and print its lifetime when it dies.
- **S3. Crash on command.** The parent sends SIGUSR1 to a chosen slot, and the worker reacts by crashing. Pick the slot from stdin, or from a second fd in the epoll loop. This is the cluster's debug trigger in miniature.
- **S4. Die with the parent.** Look up `prctl(PR_SET_PDEATHSIG)` (Linux-only) and make T8 kill the workers too. Write down why a race exists if the parent dies before the child calls it, and how to close that race.

---

## 8. Suggested layout

```
toyA/
  step1_blocking/
    supervisor.cpp
  step2_signalfd/
    supervisor.cpp
  Makefile          # -Wall -Wextra -g ; one target per step
  NOTES.md          # answers to section 6, test results, surprises
```

In NOTES.md, record for each test: what you expected, what happened, and why, if they differed. These notes become your writeup material.

---

## 9. References (look up as needed)

- `man 2 fork`, `man 2 waitpid`, `man 2 signalfd`, `man 2 sigprocmask`, `man 7 signal`
- `man 7 signal-safety` (why handlers are dangerous, and therefore why signalfd)
- TLPI (Kerrisk): ch 20-22 signals (22.11 signalfd), ch 24-26 process creation, termination, and monitoring children
- OSTEP ch 5 (Process API), which you've already covered

---

## 10. After this toy

Toy B: `mmap(MAP_SHARED | MAP_ANONYMOUS)` before fork, a shared counter race between workers, a fix with a process-shared robust mutex, then `std::atomic`. After that, the slot table from this toy moves into shared memory and becomes the in-progress marker (worker PID plus request ID) that V0 uses to requeue a dead worker's request.

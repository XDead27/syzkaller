# JTAG-backed executor forwarding in syzkaller: required source changes and migration plan

## Scope

This document describes how to modify syzkaller so that `syz-executor` remains the program decoder/driver, while all SUT-affecting operations are forwarded to an external server (assumed to exist) that executes them over JTAG.

It intentionally focuses on syzkaller-side changes only.

**Important assumptions**
- Keep the existing manager/runner protocol (`flatrpc`) intact.
- Keep `syz-manager -> syz-executor runner` orchestration intact.
- Do **not** rely on the current `executor_xtratum.h` attempt; design from first principles.
- The external server is trusted to implement the requested interface correctly.

---

## 1. Current architecture (relevant parts)

Execution pipeline today:
1. `syz-manager` starts `syz-executor runner <vm-index> <manager-host> <manager-port>`.
2. `runner` connects to manager (`pkg/rpcserver`) over `flatrpc`.
3. `runner` spawns persistent `exec` subprocesses (`executor_runner.h` / `Proc::Start`).
4. `exec` parses serialized program (`executor.cc`) and performs:
   - `instr_copyin` / `instr_copyout`
   - syscall execution
   - coverage/signal/result reporting.

The runner<->manager protocol is already mature and should remain unchanged.

---

## 2. Target architecture

Keep all control-plane behavior as-is, but replace the SUT data-plane implementation in `exec`:

- `instr_copyin` -> forwarded memory write to remote server
- syscall instruction -> forwarded syscall execution request
- `instr_copyout` -> forwarded memory read from remote server

`exec` remains the authoritative instruction parser and result-slot manager (`results[]`, `arg_result`, etc.).

This avoids invasive `proxyapp`/`rpcserver` rearchitecture.

---

## 3. New internal executor interface to introduce

Create a new executor-side abstraction (new files under `executor/`):

- `sut_backend.h`
- `sut_backend_local.cc` (default/local implementation)
- `sut_backend_remote.cc` (JTAG forwarding implementation)

Suggested interface:

```cpp
struct SutCallResult {
	intptr_t ret;
	int err_no;
	bool transport_ok;
};

class SutBackend {
public:
	virtual ~SutBackend() = default;

	virtual bool Init(/* endpoint, timeouts, mode */) = 0;
	virtual bool BeginProgram(uint64 req_id, int proc_id) = 0;
	virtual bool EndProgram() = 0;

	virtual bool CopyIn(uint64 addr, const uint8* data, uint64 size) = 0;
	virtual bool CopyOut(uint64 addr, uint64 size, uint64* value) = 0;

	virtual SutCallResult Syscall(int32 sys_nr,
		intptr_t a0, intptr_t a1, intptr_t a2,
		intptr_t a3, intptr_t a4, intptr_t a5) = 0;
};
```

### Behavioral requirements
- `CopyIn`/`CopyOut` addresses are absolute executor addresses (`SYZ_DATA_OFFSET + off` semantics preserved).
- `Syscall` must preserve `(ret, errno)` semantics exactly.
- transport failures must be distinguishable from syscall failures.

---

## 4. Required syzkaller source modifications

## 4.1 `executor/executor.cc`

### A) Add backend lifecycle
- Initialize backend once in `main()` (for `exec` mode).
- Start/end backend program scope in `execute_one()`:
  - `BeginProgram(request_id, procid)` before instruction loop
  - `EndProgram()` before returning.

### B) Route SUT-affecting operations
- `instr_copyin` path:
  - Keep current decoding logic.
  - Build concrete bytes exactly as today.
  - Send resulting write via `backend.CopyIn(addr, bytes, size)`.
- `instr_copyout` path (`copyout_call_results` / `copyout`):
  - Replace direct local memory reads with `backend.CopyOut(...)` in remote mode.
- syscall path (`execute_call`):
  - Replace direct `execute_syscall(...)` usage with wrapper:
    - local mode: existing behavior
    - remote mode: `backend.Syscall(...)`.

### C) Preserve local semantic bookkeeping
Do **not** move these semantics to server:
- `results[]`
- `arg_result` resolution (`read_result`)
- call properties interpretation in executor
- output assembly (`CallInfoRaw`, `ExecResultRaw`) and flatrpc reporting.

## 4.2 OS syscall adapters (`executor_*.h`)

Do not remove existing local syscall implementations.

Instead, add one wrapper in `executor.cc`, e.g. `execute_sut_syscall(...)`, which selects:
- local path -> existing `execute_syscall(...)`
- remote path -> backend call.

This minimizes churn in per-OS headers.

## 4.3 `executor/executor_runner.h`

Need a way to pass remote backend configuration from `runner` command to `exec` subprocesses.

Required changes:
- Extend runner CLI parsing to accept optional extra args after `<manager-port>`.
- Store these args in `Runner`/`Proc` state.
- In `Proc::Start()`, include forwarded args in `argv` for `exec` subprocess.

Current hardcoded `argv = {bin_, "exec", nullptr}` must become dynamic.

## 4.4 Runner entrypoint (`executor.cc: runner(...)`)

- Relax arg count check from fixed `argc == 5` to `argc >= 5`.
- Parse optional backend args and pass them into `Runner` constructor.

## 4.5 `syz-manager/manager.go`

In `runInstanceInner`, command construction is currently:

`<executor_bin> runner <index> <host> <port>`

Update to append optional backend args when configured, e.g.:

`... runner <index> <host> <port> --sut=remote --sut_addr=<addr> --sut_timeout_ms=<n>`

This keeps rollout explicit and reversible.

## 4.6 Configuration plumbing (manager config)

Add new optional config section (example):

```json
"sut_backend": {
  "mode": "local|remote",
  "addr": "127.0.0.1:9001",
  "timeout_ms": 200,
  "retries": 1
}
```

Plumb into manager runtime config and into executor commandline generation.

## 4.7 `vm/proxyapp` (optional but recommended)

If proxyapp is your deployment mode, add support to pass backend args/env consistently to the command it starts.

This avoids hardcoding transport endpoints into images.

---

## 5. Remote memory model requirements

Because target memory exists only behind JTAG, the executor must treat remote memory as source of truth for SUT-visible state.

Requirements:
- Every `instr_copyin` write must be reflected remotely before syscall execution.
- Every `instr_copyout` read must come from remote memory.
- `arg_csum` behavior must remain byte-accurate.

Implementation note:
- Keep executor’s current computation logic for value formation/encoding.
- Forward final write/read operations, not high-level symbolic args.

---

## 6. Error and timeout semantics

Map transport failures to executor failures clearly:
- transport/protocol failure in `Syscall`/`CopyIn`/`CopyOut` => fail current request with `ReturnError` behavior preserved.
- syscall-level failures from target are normal `(ret=-1, errno=...)` outcomes and must not be treated as transport errors.

Keep existing manager-visible statuses (`Success`, `ExecFailure`, `Hanged`) unchanged.

---

## 7. Migration plan

## Phase 0: Interface scaffolding (no behavior change)
- Add `SutBackend` abstraction.
- Implement `LocalSutBackend` using current direct memory/syscall code paths.
- Wire executor to always use local backend.

**Exit criteria**: no functional diffs in existing tests.

## Phase 1: Remote syscall-only
- Implement `RemoteSutBackend::Syscall`.
- Keep copyin/copyout local for now.

**Purpose**: validate protocol, retries, timeout/error mapping.

**Exit criteria**: stable syscall forwarding on synthetic programs with scalar args.

## Phase 2: Remote copyin/copyout (required for JTAG memory)
- Route `instr_copyin` writes to `RemoteSutBackend::CopyIn`.
- Route `instr_copyout` reads to `RemoteSutBackend::CopyOut`.
- Keep local `results[]` logic unchanged.

**Exit criteria**: pointer-heavy programs execute correctly; `arg_result` dependencies preserved.

## Phase 3: Manager/config/runner plumbing
- Add manager config section.
- Add command-line propagation runner->exec.
- Implement runtime mode selection (`local` vs `remote`).

**Exit criteria**: feature flag controlled rollout per target/manager instance.

## Phase 4: Hardening and performance
- Add batching support in backend transport (optional API extension).
- Add counters/telemetry for transport failures and latency buckets.
- Tune timeouts for JTAG latency.

**Exit criteria**: acceptable exec/s throughput and no systematic false hangs.

---

## 8. Validation plan

## Functional parity tests
- Resource chaining via `arg_result`.
- Mixed `instr_copyin` kinds: `arg_const`, `arg_addr32/64`, `arg_data`, `arg_csum`.
- `instr_copyout` with sizes 1/2/4/8.
- Fault injection and rerun call props unaffected.

## Robustness tests
- Server disconnect mid-program.
- Server timeout on syscall.
- Partial copyin/copyout failure.

## Manager integration tests
- VM startup and runner handshake unchanged.
- `ExecResult`/coverage parsing unchanged.
- Repro pipeline still works with remote backend enabled.

---

## 9. Recommended non-goals for first rollout

Do not change in initial implementation:
- `flatrpc` protocol between manager and runner.
- scheduler/queue logic in `pkg/rpcserver/runner.go`.
- coverage transport format.
- proxyapp RPC API for execution requests.

These remain stable and reduce regression risk.

---

## 10. Summary

For JTAG-only targets, the least risky architecture is:
- keep syzkaller control plane unchanged,
- introduce a strict executor-internal `SutBackend` abstraction,
- forward `copyin/copyout/syscall` through that backend,
- roll out via feature flag and staged migration.

This provides the required target-memory control while avoiding a high-risk rewrite of manager/runner/proxyapp execution protocols.

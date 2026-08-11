# Cross-Thread Test Callback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Ensure every GPU-server test result is actually delivered to the
originating development task before the testing task claims that it reported
the result.

**Architecture:** The delegated request supplies the source thread ID. The
testing task explicitly sends its complete result to that thread and verifies
the tool response; the development task waits on the testing thread only as a
fallback.

**Tech Stack:** Codex app thread tools, repository `AGENTS.md` rules.

## Global Constraints

- The testing task, not the development task, performs remote synchronization
  and execution.
- PASS, FAIL, BLOCKED, and partial results all require the same callback.
- `RESULT_NOTIFICATION` describes callback delivery only; `TEST_RESULT`
  describes synchronization, build, or test outcomes.
- A handshake never replaces the delegated test; the testing task continues
  the requested remote commands after sending it.
- A statement in the testing task's own final response is not delivery.
- No product code, remote deployment, Git commit, or Git push is authorized by
  this workflow change.

---

### Task 1: Define verifiable result delivery

**Files:**
- Modify: `AGENTS.md`

**Interfaces:**
- Consumes: `codex_delegation.source_thread_id` from the delegated request.
- Produces: a successful `send_message_to_thread` result targeting that ID, or
  an explicit `RESULT_NOTIFICATION=FAIL` report after one retry.

- [x] Require an explicit `send_message_to_thread` call for every completed
  test run.
- [x] Require checking the returned target before claiming delivery.
- [x] Define one retry and the failure report when delivery cannot complete.
- [x] Define `wait_threads` as a fallback rather than the primary callback.

### Task 2: Verify the callback path

**Files:** No repository file changes.

**Interfaces:**
- Consumes: this task's source thread ID.
- Produces: a callback handshake visible in the originating development task.

- [x] Send the updated rule to `GPU服务器代码测试`.
- [x] Require the testing task to call `send_message_to_thread` with a handshake
  before resuming Task 8C.
- [x] Confirm that the originating task receives the handshake and that the
  returned target ID matches.

### Task 3: Resume Task 8C

**Files:** No product-code changes.

**Interfaces:**
- Consumes: verified callback path and commit `0c021bf`.
- Produces: complete Task 8C PASS/FAIL/SATURATED results delivered by callback.

- [ ] Continue the all-valid two-Station UDP aggregate-rate scan.
- [ ] Require the complete result report to use the verified callback path.
- [ ] Keep Task 8D blocked until the Task 8C highest stable rate is known.

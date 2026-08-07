# Repository agent instructions

## Visual generation validation

- After every image or diagram generation/edit, inspect the resulting artifact
  at readable resolution before accepting or delivering it.
- When a visual contains Chinese text, verify every Chinese title, label,
  annotation and legend against the requested source text. Check specifically
  for mojibake, pseudo-Chinese glyphs, wrong characters, missing text and
  duplicated text.
- If any Chinese text is incorrect or unreadable, iterate on the visual and
  repeat the inspection. Do not save, reference or deliver that version as the
  final artifact.
- Report whether the final Chinese-text inspection passed.

## Agent skills

### Server test handoff

- After completing any new module, feature or behavior change, notify the
  existing Codex task named `GPU服务器代码测试` and identify exactly which
  modules, code paths and behavior require testing. This applies whether the
  implementation uses GPU, CPU, PSRDADA/RDMA, scripts, configuration or other
  project code.
- A successful macOS/local test is development evidence only and never replaces
  testing on the GPU server. Every completed feature must be handed off for
  GPU-server build and test before it is considered accepted.
- The handoff must include enough context for the testing task to act without
  rediscovery: the current branch or revision when known, affected files or
  modules, relevant build and test commands, test inputs or fixtures, and the
  expected results or acceptance criteria.
- The development task is responsible only for sending the test requirements.
  The `GPU服务器代码测试` task is responsible for synchronizing the code to the
  remote GPU server, running the tests and reporting the results.
- After the `GPU服务器代码测试` task explicitly reports that the requested tests
  passed, remind the user that the tested changes are ready for a Git commit.
  Do not create the commit automatically; the user decides when to commit.
- The user always makes the final decision about whether to push a tested commit
  to GitHub. Do not push it automatically or interpret a passing test result as
  authorization to push.
- Do not treat this rule as authorization for the development task to commit,
  push, deploy, access the remote server or run remote tests.
- If the target task cannot be found or the notification cannot be delivered,
  report that failure explicitly to the user; do not claim that the server test
  handoff was completed.

### Remote server test execution

- Before designing, writing or changing a remote test program, controller or
  script, run a read-only Phase 0 preflight on every target server and return
  the evidence to the development task. Record exact executable paths,
  supported options, versions, dynamic-library resolution, permissions,
  NIC/link/NUMA state, available CPU/memory/disk capacity, clock status and
  relevant shell/command behavior. Design the harness against those observed
  facts rather than assumed PATH contents or local command semantics.
- If Phase 0 is incomplete or a required facility is absent or incompatible,
  report `TEST_RESULT=ENV_BLOCKED` with the failed command and propose the
  smallest environment decision needed. Do not proceed by guessing paths,
  silently installing software or writing workarounds for an unverified
  environment. After an approved environment change, repeat Phase 0 before
  starting the test implementation or formal run.
- Git version control is owned by the local development task. Test hosts are
  not required to have Git installed, and the `GPU服务器代码测试` task must not
  run Git commands, create commits, change branches or push from HF/qths/qtp
  servers. Verify synchronized sources using the file SHA256 manifest supplied
  by the development task; record configuration and binary SHA256 values in
  the test artifacts.
- A delegated test request contains a `codex_delegation.source_thread_id`.
  The `GPU服务器代码测试` task must copy that exact ID and, after every test
  run, call the Codex app `send_message_to_thread` operation with the complete
  result report and that source thread as the target. Writing the report only
  in the testing task's own final response, mentioning that it was returned,
  or relying on `source_thread_id` metadata does **not** count as delivery.
- Delivery is successful only when the `send_message_to_thread` operation
  returns successfully and identifies the intended source thread. The testing
  task must not claim that results were returned before checking this tool
  result. If delivery fails, it must retry once after resolving the source
  thread again; if the retry also fails, its own final response must state
  `RESULT_NOTIFICATION=FAIL` and include the target thread ID and error.
- Notification status and test status are independent. A successfully
  delivered callback must state `RESULT_NOTIFICATION=PASS`, even when the test
  itself is `TEST_RESULT=FAIL`, `TEST_RESULT=BLOCKED`, or
  `TEST_RESULT=INCOMPLETE`. `RESULT_NOTIFICATION=FAIL` is reserved exclusively
  for failure to deliver the callback after the required retry; it must never
  be used to describe a test, synchronization, build, or environment failure.
- A callback handshake verifies only the notification path. It is not a
  completed test run and must not terminate, replace, or postpone the delegated
  test request. After a successful handshake, continue the requested remote
  commands in the same turn. Do not return `TEST_RESULT=INCOMPLETE` merely
  because the requested test has not been started. `INCOMPLETE` or `BLOCKED`
  requires either an explicit user stop or evidence from an attempted command
  showing an external blocker; include that command and its key output.
- The originating development task should use `wait_threads` while a delegated
  test is active. A wait timeout is not a test result: keep the test status
  pending and later resume from the returned cursor. Explicit delivery by the
  testing task is the primary completion signal; polling is only a fallback.
- After **every** test task or test run completes, the
  `GPU服务器代码测试` task must automatically send a result report to the
  originating `模块开发` task, regardless of whether the outcome is PASS,
  FAIL, BLOCKED, or only partially completed. This report is mandatory even
  when an earlier synchronization, build, environment, or test stage fails;
  it must identify the first failed stage and the current cleanup state.
- When a test request is received from the `模块开发` task, the
  `GPU服务器代码测试` task must complete the requested synchronization,
  build, diagnostics and tests, then automatically return the complete result
  to the originating `模块开发` task in the same task flow. The report must
  state PASS or FAIL for each requested stage and include blocking conditions,
  key commands and relevant logs.
- When the `模块开发` task reports that a module or feature is complete and
  requests testing, the `GPU服务器代码测试` task must synchronize the latest
  development worktree to the configured remote test server before testing.
- The testing task must build and run every requested CPU, GPU, PSRDADA/RDMA,
  integration, script or configuration test on the remote GPU server, then
  return an explicit success or failure result to the originating `模块开发`
  task.
- A failed synchronization, build, test, environment check, or notification
  must be reported explicitly as a failure, including the failed stage,
  relevant command and complete key logs; never imply success when testing did
  not complete.
- Do not modify product implementation code while performing this testing
  workflow. Preserve unrelated remote files and existing build/data outputs
  during synchronization.

### Reproducible test acceptance

- A one-off successful command is development evidence, not final acceptance.
  Final CPU, GPU, RDMA/PSRDADA, integration and performance tests must use a
  versioned repository test runner and follow `docs/agents/testing.md`.
- Every final acceptance run must be reproducible from a clean state using one
  documented entry command, deterministic or recorded inputs, scoped resource
  ownership, persistent logs and a machine-readable result artifact.
- Functional acceptance requires at least three consecutive clean repetitions.
  Performance acceptance requires a warm-up followed by at least three
  measured repetitions at every rate point. Report every repetition and the
  aggregate statistics; never select only the best run.
- A temporary `/tmp` script, an interactive shell history, or manually assembled
  nested SSH command cannot be the authoritative test runner. Temporary remote
  files may be generated only by the versioned runner and must be traceable to
  its commit and recorded configuration.
- Keep test outcome, notification delivery and cleanup status independent.
  Cleanup must never overwrite PASS/FAIL/BLOCKED evidence. A harness failure
  must be reported as `HARNESS_FAIL`, not as a product or performance failure.
- Do not claim a controller, fixture or test path is ready from a dry-run alone.
  Its real execution path, failure path, resume path and scoped cleanup must be
  covered by automated tests before it controls server acceptance.
- Design integration and performance tests around the continuous astronomical
  observation semantics in `docs/agents/testing.md`. In particular, all
  configured Stations are mandatory: if one sender cannot start or exits
  abnormally, abort the complete transfer and stop the receiver-side pipeline.
  Treat low-rate packet loss/zero-fill separately from Station participation.
- Use `dada_dbdisk` only when the acceptance objective requires inspecting the
  materialized compute `.dada` file. For high-throughput pipeline measurements,
  use `dada_dbnull -s -z` to drain the compute ring without introducing disk
  throughput as an unrelated bottleneck, and validate its clean EOD exit.

### Issue tracker

Issues and PRDs are tracked in GitHub Issues for `hyysky/rdma_dada`. See
`docs/agents/issue-tracker.md`.

### Domain docs

This repository uses a single-context domain-doc layout. See
`docs/agents/domain.md`.

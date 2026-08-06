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

### Issue tracker

Issues and PRDs are tracked in GitHub Issues for `hyysky/rdma_dada`. See
`docs/agents/issue-tracker.md`.

### Domain docs

This repository uses a single-context domain-doc layout. See
`docs/agents/domain.md`.

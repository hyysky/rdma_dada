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

### Issue tracker

Issues and PRDs are tracked in GitHub Issues for `hyysky/rdma_dada`. See
`docs/agents/issue-tracker.md`.

### Domain docs

This repository uses a single-context domain-doc layout. See
`docs/agents/domain.md`.

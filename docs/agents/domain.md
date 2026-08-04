# Domain Docs

Engineering skills should consume this repository's domain documentation
before exploring or changing the affected area.

## Before exploring, read these

- `CONTEXT.md` at the repository root, when present.
- Relevant ADRs under `docs/adr/`, when present.

Missing domain documents are not an error. Proceed silently. Domain-modeling
workflows create them when terminology or architectural decisions are resolved.

## Layout

This is a single-context repository:

```text
/
├── CONTEXT.md
├── docs/
│   └── adr/
└── src/
```

## Vocabulary

Use domain terms as defined in `CONTEXT.md`. Avoid replacing established terms
with unrecorded synonyms.

## ADR conflicts

If proposed work conflicts with an existing ADR, report the conflict explicitly
instead of silently overriding the decision.

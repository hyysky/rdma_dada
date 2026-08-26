# Accepted Test Evidence

`accepted-results.json` is the small, Git-tracked index of test suites that the
user has explicitly selected as durable project or publication evidence. It is
not a copy of runtime logs and it is not an assertion that every selected suite
passed: a reproducible failure boundary may also be accepted as evidence.

The authoritative runtime artifacts remain under the ignored local
`test-results/suites/<suite-id>/` directory. Entries in this file are copied
only from a verified generated catalog by `scripts/task8c_catalog.py promote`.
Do not edit catalog entries manually, and do not promote a suite without the
user's explicit approval. Each promoted item also retains the verified
`origin` object, including the source host and exact absolute remote suite
path, so the selected evidence remains externally traceable after runtime
catalogs are cleaned or moved.

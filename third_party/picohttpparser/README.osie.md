# picohttpparser (vendored)

- Upstream: https://github.com/h2o/picohttpparser
- Commit: 465a7ff09fbd3432fe56c673451f5460154d1f07 (2026-09-29)
- Files: `picohttpparser.c`, `picohttpparser.h`, unmodified
- License: MIT (or the Perl license, at the user's choice); see `LICENSE`

Used by the agent's local API (`src/api/http.cpp`) to parse HTTP/1.x request
lines and headers only. It does no I/O and no allocation. OSIE does the
rest itself: socket handling, deadlines, body framing (Content-Length only;
chunked transfer coding is rejected) and header policy.

Why this parser: it is about 800 lines, has no dependencies, and is the
HTTP/1 parser of the H2O web server and of several widely used Perl HTTP
servers. It is fuzzed by OSIE's `osie_fuzz_api` harness through the API
request path.

SHA-256 of the vendored files:

```
6480689711996bc9a219e26df27ac32ebbb7dce2f24a63a316d041b08f646199  picohttpparser.c
1fc9074dd12418b2b91e55ef3a8279bccf8d3577b790dea6f2f908ab2a157f1c  picohttpparser.h
```

# Simple Binary Encoding tool, vendored

| | |
|---|---|
| Upstream | https://github.com/aeron-io/simple-binary-encoding |
| Version | 1.40.1 (2026-08-21) |
| Artifact | `uk.co.real-logic:sbe-all:1.40.1` from Maven Central |
| URL | https://repo1.maven.org/maven2/uk/co/real-logic/sbe-all/1.40.1/sbe-all-1.40.1.jar |
| Checksum | `sbe-all-1.40.1.jar.sha256`; Maven's sha1 was `530deaceecdb438e177ceafbe61023f9338a32a6` and matched |
| Needs | a JVM (Java 17+; this machine has 21) — only for `make regenerate` / `make check-generated` |

The generated C++ headers are checked in under `generated/`
(`docs/spec.md` §3.2): the checked-in copy is what compiles, the CI
diff is what keeps it honest. A build machine without Java still
builds.

To bump: download the new jar, write its sha256 next to it, update
this file, `make regenerate`, review the diff of `generated/`, commit
all of it together.

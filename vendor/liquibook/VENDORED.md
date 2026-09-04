# liquibook, vendored

| | |
|---|---|
| Upstream | https://github.com/enewhuis/liquibook |
| Commit | `2427613b32f1667abae68a01df6af9ba8270f8e7` (master, 2022-12-15) |
| License | BSD-3-Clause, Object Computing, Inc. — `license.txt` |
| What | `src/book/*.h` only: the header-only matching core |
| What not | MPC build files, tests (Boost), examples, `main.cpp`, docs |

Refreshed by:

```
git clone https://github.com/enewhuis/liquibook /tmp/liquibook
git -C /tmp/liquibook checkout <commit>
cp /tmp/liquibook/src/book/*.h vendor/liquibook/src/book/
cp /tmp/liquibook/license.txt vendor/liquibook/
```

then update the commit above and re-run `make check-liquibook`, which
greps this copy for the constructs `docs/spec.md` §5 forbids on the
apply path and diffs the I/O hits against `io-allowlist.txt`. Any new
hit is a determinism review, not a whitespace change.

Compiled as a `SYSTEM` include (`exchange::liquibook`), so this
repository's `-Werror` does not apply to upstream code.

# How Claude reports a test (gates 4 and 5)

The five gates from switch-port-base, as set up in this repo:

1. **Build only the latest push**: `setup/build_pc.ps1` fetches GitHub's main, fast-forwards, prints the commit it
   builds, and builds nothing if it cannot. The program's log starts with `[build] <commit>`.
2. **Preflight before build commands**: `tools/preflight.py` (tree, codegen, compile with the owner's clang and SDL2,
   hardware sweep, address list). The owner gets build commands only with `RESULT: PASS`, quoted in the reply.
3. **Logs read by script first**: `tools/read_log.py LOG` stops with WRONG BUILD unless the log's `[build]` line is
   the latest push, then prints the facts with line numbers.
4. **Report shape** (every report on a test):
   - **Build**: the commit, and that it matches the push.
   - **What the log shows**: each fact with its source line.
   - **What it means**: a cause only when proven; otherwise what the next test checks.
   - **Not checked**: what this run could not tell us.
   - **Next**: the plan, waiting for the owner's go-ahead.
   Never "worked", "fixed", "in your build" or "will show" without the line that proves it.
5. **Independent check at the big handoffs** (first boot test, a release, the test after two failed ones): a
   separate agent that has not seen the work marks every claim in the draft message SUPPORTED, UNSUPPORTED or
   WRONG against the repo, the preflight report and the logs. Unsupported claims are fixed or removed first.

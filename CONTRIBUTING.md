# Contributing

Thanks for wanting to improve the display. Small, well-explained changes are the easiest to merge.

## Set up

1. Simulator and tests: [docs/build-guide.md](docs/build-guide.md), section on the simulator.
   `cmake -S sim -B build/sim && cmake --build build/sim -j && ctest --test-dir build/sim --output-on-failure`
2. Helper: `cd helper && npm ci && npm test && npm run typecheck`
3. Firmware (needs PlatformIO): `cd firmware/app && pio run -e app`

Most work happens in `ui/` and is testable on a PC without a board. Please start there.

## What a good pull request looks like

- **One idea per pull request.** Under about 1500 changed lines; bigger ones are hard to review.
- **A title in the form `type(scope): what changed`**, for example `feat(ui): show red cards on the pitch`.
  Types: feat, fix, docs, test, refactor, perf, build, ci, chore, revert.
- **A description that answers three questions** (the template asks them): what changes, why, and how you tested it.
- **A test.** Changes in `ui/src/` come with a test in `sim/tests/`; changes in `helper/src` or `helper/api` with one in
  `helper/test/`. Fix a bug by writing the failing test first.
- **For firmware changes,** say which board revision you flashed and what you saw. The simulator cannot show timing,
  memory or sound.
- **No secrets.** Never commit `secrets.ini`, `.env` files, tokens, Wi-Fi names or passwords. The helper token is
  provisioned over serial and lives only in the device's flash.

## Rules that protect the device

- `ui/` stays pure: no hardware calls, no clock reads, no threads, no `printf`. Hardware sits behind the interfaces in `ui/src/`.
- Internal RAM is scarce. Big buffers go to PSRAM; keep at least about 60 KB of internal RAM free during a secure connection.
- The release build (`env app`) must not contain diagnostics or plain-http code (CI checks this).
- Opponents and other teams never make sound, and a sound must never startle someone in a quiet room.

## What the automatic checks do

| Check | What it looks for |
|---|---|
| Simulator tests | All doctest cases pass |
| Sanitizers | No memory or undefined-behaviour errors in our code |
| Helper | TypeScript compiles, vitest passes |
| Firmware builds | All four environments build; the release image has no diagnostics |
| Docs and tooling | Relative links resolve, the check scripts pass their own tests |
| Pull request checks | Title format, a filled-in description, a test next to code changes, no forbidden files, sensible size |
| Secret scan | gitleaks over the commits you add |

Run the pull request checks yourself before opening one:

    PR_TITLE="fix(ui): ..." PR_BODY="$(cat my-description.md)" python3 tools/ci/pr_checks.py origin/main HEAD

If a check is wrong for your change, say so in the pull request; a maintainer can waive a test requirement with the
`no-tests-needed` label or a size limit with `large-change`.

## When a build breaks

- On your pull request, a bot comment mentions you with the failed jobs and the command that repeats each one locally. Push a fix and the comment updates.
- On `main`, an issue is opened and assigned to whoever pushed the commit and to the maintainer; it closes itself when `main` is green again.
- If the failure looks like a problem with the build environment (a download or the runner) the jobs are retried once; if it persists only the maintainer is told, not you.
- A weekly run catches breakage from outside (package updates) even when nobody pushed.

## Reporting bugs and ideas

Use the issue templates. For a board that behaves differently from the one in the README, the "It works (or not) on my
board" template is very welcome.
[broken](docs/does-not-exist.md)

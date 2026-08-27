# Contributing

This is a personal firmware project for one specific board. Issues and small,
focused pull requests are welcome; large redesigns probably are not — open an
issue first so neither of us wastes an afternoon.

## Ground rules

- Read `README.md` (build + flash) and `docs/architecture.md` before changing
  anything under `src/`.
- Keep to the surrounding style: no new abstractions unless they delete more
  code than they add, comments explain *why*, not *what*.
- One logical change per pull request, with a commit message that says what
  changed and why.
- Verify on hardware when you can. If you cannot, say so in the PR — a
  compile-only change is fine as long as it is labelled as one.
- Never commit credentials. Wi-Fi details are entered on the device and stored
  in NVS; see `SECURITY.md`.

## Pull requests from forks

There is no CI on this repository today. If workflows are added later, they run
against fork pull requests **only after manual approval**, and they never get
access to repository secrets. Do not add a workflow that uses
`pull_request_target`, `workflow_run`, or `issue_comment` to build or execute
code from a pull request branch — those triggers run with a writable token in
the base repository's context and turn any fork PR into arbitrary code
execution. Use `pull_request`.

If you do add a workflow:

- set a minimal top-level `permissions:` block (`contents: read` unless more is
  genuinely needed);
- pin every third-party action to a full commit SHA, not a tag;
- do not interpolate untrusted values (`github.event.pull_request.title`, branch
  names, issue bodies) directly into `run:` blocks — pass them through `env:`.

## Reporting security issues

Do not open a public issue. Follow `SECURITY.md`.

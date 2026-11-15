# Security policy

Pure is pre-1.0 research software. It has had no external security audit and is not meant for deployment yet.

## Reporting

Please do not open a public issue for a vulnerability. Use GitHub's private reporting:
**Security → Report a vulnerability** on this repository. Include what you ran, what you saw, and the commit.

We acknowledge a report within 7 days. A fix lands with a regression test and an advisory.

## Advisories

Fixed issues are summarised in [`docs/security/`](docs/security/README.md), one advisory per issue, with the fixing commit
and the test that locks it. Advisory numbers are shared with [aup](https://github.com/yeowoo-labs/aup) and
[Vega](https://github.com/yeowoo-labs/vega).

## Supported versions

Only `master` is supported. A preview release is a snapshot and gets no fixes of its own.

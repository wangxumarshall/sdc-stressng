# Support

## Documentation first

| Question type | Where to look |
|---|---|
| What this tool is, how to run it | [README](README.md) |
| Architecture and design decisions | [docs/architecture.md](docs/architecture.md) |
| Excitation methodology, coverage matrix | [docs/excitation-guide.md](docs/excitation-guide.md) |
| Running joint campaigns with the detector | [docs/sdcshield-integration.md](docs/sdcshield-integration.md) |
| Upstream merge policy | [docs/upstream-sync.md](docs/upstream-sync.md) |
| Every option | `man stress-ng` / `./stress-ng --help` |

## Issues

Open an issue with the appropriate template (bug or feature request). For
bugs, always include:

- the exact command line
- the stress-ng version (`./stress-ng --version`)
- CPU model, architecture features (`lscpu`), kernel
- whether `--verify` was enabled and what it reported

## Out of scope here

- **SDCShield questions** (detection, golden cases, cpu-mask reports) belong
  in the SDCShield repository, not here — this repo is the excitor.
- **Generic upstream stress-ng questions** (non-arm64 platforms, packaging,
  upstream features) belong in
  [ColinIanKing/stress-ng](https://github.com/ColinIanKing/stress-ng).
- **Security issues**: see [SECURITY.md](SECURITY.md) — never a public issue.

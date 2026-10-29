# Project rules for coding agents

## Repository and branches

- Before work, verify the repository root, current branch, working-tree status,
  relevant remotes, and the target hardware/environment.
- Keep the public project in `Moominza/FluidDial-JC`; keep
  `bdring/FluidDial` as the upstream source. Preserve upstream Git history.
- `main` is for reviewed, stable milestones. Active JC development belongs on
  `feature/jc8048w550`. Give future feature branches a separate, documented
  purpose.
- Do not rewrite published history, force-push, or merge unfinished work into
  `main`. Do not push an initial public checkpoint until the owner has reviewed
  the exact repository, branch, files, and contents to be published.
- After that first publication is approved, routine pushes of tested
  development checkpoints to the designated feature branch are permitted.
  Merging to `main` and publishing releases require separate explicit approval.

## Compatibility, hardware, and safety

- Preserve original upstream behavior and supported targets unless a deliberate,
  documented change is approved.
- Keep JC-specific hardware, display, touch, and user-interface changes
  isolated from M5Dial and CYD targets.
- Document technical decisions, verified board identity/revision, pin
  assignments, compatibility limits, and physical test evidence.
- Never modify or flash FluidNC/CNC controller firmware or configuration
  without explicit approval.
- Do not add, enable, or transmit machine-motion commands without explicit
  approval and a separate safety review. A successful build or status-only test
  is not permission to move a machine.
- Keep demo/simulated behavior clearly distinguished from live controller
  state. Do not silently enable controller communication or motion.

## Security, licenses, and reproducibility

- Never commit credentials, private data, local configuration, or secrets in
  source, documentation, test fixtures, CI configuration, or logs.
- Keep generated build products and temporary files out of commits.
- Preserve the upstream GPL-3.0 license, copyright attribution, and all
  third-party license notices, including font and driver licenses.
- Pin external dependencies where practical and keep build steps documented
  and reproducible.
- Do not expose secrets in CI logs. Use no repository secrets unless a reviewed
  workflow genuinely requires them.

## Verification and reporting

- Run the smallest relevant tests/builds for a change; run all three supported
  PlatformIO environments (`jc8048w550`, `m5dial`, `cyddial`) for shared or
  checkpoint changes.
- Report the exact commands and results. Never claim a test, upload, boot,
  visual review, or physical hardware check passed unless it actually ran and
  evidence supports the claim.
- Clearly separate compile success, upload success, boot verification, and
  operator-confirmed hardware results.
- Review the complete intended diff for credentials, private information,
  generated artifacts, license omissions, and unrelated changes before
  staging or publishing. Do not discard unrelated or pre-existing local work.

## Required instructions

Read this file before changing project files. `CLAUDE.md` points Claude to
these same rules; keep that reference intact.

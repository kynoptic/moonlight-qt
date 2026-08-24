---
Description: Consolidated AGENTS.md template combining structure, rules, and concrete examples. Adapt and trim to your project. AGENTS.md is canonical file; CLAUDE.md should be a symlink to it (ln -s AGENTS.md CLAUDE.md).
---

# `AGENTS.md` – Project guide for AI agents

> **File convention**: This `AGENTS.md` is the canonical project instructions file. `CLAUDE.md` should be a symlink pointing here (`ln -s AGENTS.md CLAUDE.md`), not a separate file.

## Project overview and goals

[Briefly describe what the project does, who uses it, and what the AI agent is expected to accomplish (e.g., implement features, fix bugs, write tests, maintain docs).]

## Repository structure

- `[directory]/` – [Purpose and contents]
- `[directory]/` – [Purpose and contents]
  - `[key-file]` – [Constraints or notes]
  - ...

### Key files

- `[filename]` – [Purpose and usage]
- `[filename]` – [Critical restriction or behavior]

## Environment setup and commands

- **Install dependencies**:

  ```bash
  [install command]
  ```

- **Run application / service**:

  ```bash
  [run command]
  ```

- **Run tests**:

  ```bash
  [test command]
  ```

- **Lint and format**:

  ```bash
  [lint command]
  [format command]
  ```

- **Build / package (optional)**:

  ```bash
  [build command]
  ```

- **Database / migrations (if applicable)**:

  ```bash
  [migration command]
  ```

## Conventions

Only what a linter cannot enforce and a reader of the code would get wrong — a layering rule, a naming scheme the tooling doesn't check, an idiom this codebase departs from the framework's default on. Style a formatter already fixes belongs in the formatter's config, not here.

- `[convention]` – [What it looks like, and what breaks without it]
- `[convention]` – [What it looks like, and what breaks without it]

## Tools and capabilities

- [Tool or integration]: [How to use; config path or env var]
- [API/SDK]: [Auth via env var; mock in tests]
- [Browser/automation/test tool]: [Where examples live]

## Constraints and safety rules

Assumes house conventions: the Conventional Commits format and type taxonomy, the branch and PR policy, and the staging discipline from the always-loaded Git rules. Don't restate them here — replace the placeholders below with what this agent's domain adds on top.

### Safety boundaries

These are absolutes because the failure is unrecoverable, not because emphasis helps:

- **NEVER** commit secrets or sensitive data; use env vars and `.env.example`.
- **NEVER** modify generated artifacts or production configs without approval.
- **Don't** make irreversible external calls in tests (payments, emails) — use sandboxes or mocks.
- **Don't** bypass tests, lints, or quality gates to get green. Fix the root cause, not the symptom.

### Project-specific constraints

Replace these with the pitfalls a competent contractor could not infer from reading the repo — an obscure build step, a module that must not be imported directly, a test that is foundational for reasons the code doesn't show. Delete any line here that merely restates a default:

- `[constraint]` – [The trap, and what it costs when someone falls in]
- `[constraint]` – [The trap, and what it costs when someone falls in]

### Project gates

- The gate a change must clear before it lands: `[command]`, and what a red result means here.
- Where architectural decisions are recorded: `docs/adr/`.

## Known issues and context

- [Flaky tests, in-progress migrations, legacy modules not to be edited, performance caveats, etc.]

## Example tasks

- Add health endpoint
  - Files: `routes/health.[js|py]` (route), register in app entry, test in `tests/health.*`.
  - Return `{ status: 'ok', uptime, version }`; include negative tests.

- Fix calculation bug
  - Reproduce with a failing test in `tests/[module].test.*` covering edge case.
  - Update `services/[module].*`; preserve API contracts; add regression test.

---

<!-- Trim sections that don’t apply to your stack. Keep commands accurate and runnable. -->

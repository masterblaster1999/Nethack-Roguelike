# AGENTS

## Skills

The local agent instruction index is available in the root prompt context.

### Available skills
- `skill-creator`: Guide for creating effective skills. Use when creating a new skill or updating one that extends Codex capability with specialized knowledge, workflows, or integrations. (file: `C:/Users/et_pj/.codex/skills/.system/skill-creator/SKILL.md`)
- `skill-installer`: Install Codex skills into `$CODEX_HOME/skills` from a curated list or a GitHub repo path. Use when users ask to list installable skills, install a curated skill, or install a skill from another repo (including private repos). (file: `C:/Users/et_pj/.codex/skills/.system/skill-installer/SKILL.md`)

## Instruction hierarchy
- Follow this file as the repo-local baseline.
- Apply specific skill `SKILL.md` rules when relevant.
- Respect the user’s latest request when it directly conflicts.

## Small improvement / WIP policy
- For "small improvement", "WIP", "small upgrade", or similar prompts, prefer a minimal, scoped change.
- Prefer one-file changes unless multiple files are required for correctness.
- Avoid broad refactors or behavior changes unrelated to the ask.
- If the request is ambiguous, ask one short clarifying question before editing.
- After editing, summarize behavior changes in one concise sentence and avoid cleanup churn.

## Response conventions
- Keep updates concise.
- Use inline code formatting for file references.
- Prefer direct, action-oriented summaries.

## AGENTS maintenance
- Keep this file compact and actionable.
- Expand only when it improves execution consistency.
- Do not duplicate large policy text from elsewhere.

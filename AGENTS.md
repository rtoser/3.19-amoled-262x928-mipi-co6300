# AGENTS.md

## Agent skills

### Issue tracker

Issues 与 spec 放在 GitHub fork `rtoser/3.19-amoled-262x928-mipi-co6300` 的 Issues（`gh` CLI）。`origin` 是 Gitee 上游，`gh` 无法从它推断仓库；所有 `gh` 命令依赖 `gh repo set-default` 设好的默认仓库。See `docs/agents/issue-tracker.md`.

### Triage labels

五个默认标签按原名使用：`needs-triage` / `needs-info` / `ready-for-agent` / `ready-for-human` / `wontfix`. See `docs/agents/triage-labels.md`.

### Domain docs

Single-context：根目录 `CONTEXT.md`（按需惰性创建）+ `docs/adr/`. See `docs/agents/domain.md`.

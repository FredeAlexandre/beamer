# Cutting a milestone's tasks

You cut the `test` and `impl` tasks of one milestone at a time, from the rules stage 0 wrote in
`spec/<domain>.md`. The epic's Milestones section (#24) names each milestone's slice. Take only
the rules that slice needs.

## The unit

- **One rule group in one crate**: related rule ids under one `r[domain.topic.*]` prefix,
  implemented in one crate. A crate's skeleton cites the rule ids each public item serves; that
  is how a group finds its crate.
- **Split** a group above about 25 rules, or one whose implementation you expect above 1,500
  lines of diff.
- **A pair per group**: a `build:test` task and a `build:impl` task, with the test task blocking
  the impl task.
- **Bottom-up**: an impl task is also blocked by the impl tasks of the lower crates' groups it
  calls into, so crates land in dependency order within the milestone.
- Each `machine`-tier rule in the slice is tested through the machine tier (`xtask machine`) and
  each `human`-tier rule goes to the milestone checklist. Neither gets a `ci` test task.

## The issues

Title them `M<n> test: <crate> <domain.topic>` and `M<n> impl: <crate> <domain.topic>`. Each
body follows the shape of the existing tasks: a Parent section naming #24, What to build (the
crate and the rule-id prefix, with a pointer to `spec/<domain>.md`, not the rules themselves),
and Acceptance criteria:

- test: every rule in the group has a test carrying `// r[verify <id>]` that panics against the
  skeleton, through the highest seam that reaches the rule.
- impl: the group's tests pass, and every in-diff mutant is killed by `tests/`.

Create each issue, make it a sub-issue of #24, then add its blocked-by edges:

```sh
url=$(gh issue create -R FredeAlexandre/beamer --title "<title>" --label build:test --body-file body.md)
id() { gh issue view "$1" -R FredeAlexandre/beamer --json id --jq .id; }
gh api graphql -f query='mutation($p:ID!,$c:ID!){addSubIssue(input:{issueId:$p,subIssueId:$c}){issue{number}}}' \
  -f p="$(id 24)" -f c="$(id "$url")"
gh api graphql -f query='mutation($i:ID!,$b:ID!){addBlockedBy(input:{issueId:$i,blockingIssueId:$b}){issue{number}}}' \
  -f i="$(id <impl issue>)" -f b="$(id <test issue>)"
```

Done when every rule of the slice is in exactly one test task, and `queue.sh frontier` shows the
milestone's first test tasks.

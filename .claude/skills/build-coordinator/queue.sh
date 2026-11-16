#!/usr/bin/env bash
# The build queue as the coordinator reads it: what waits for Alex, what is in flight, and the
# frontier (open, unblocked, unclaimed tasks). Read-only: it changes nothing on GitHub.
#
#   .claude/skills/build-coordinator/queue.sh            # all three sections
#   .claude/skills/build-coordinator/queue.sh frontier   # one of: alex, flight, frontier
set -euo pipefail

REPO=FredeAlexandre/beamer
EPIC=24
section=${1:-all}

# Every sub-issue of the epic, one JSON object per line, with its labels, assignees and blockers.
tasks() {
  gh api graphql --paginate -f query='
    query($endCursor: String) {
      repository(owner: "FredeAlexandre", name: "beamer") {
        issue(number: '"$EPIC"') {
          subIssues(first: 100, after: $endCursor) {
            pageInfo { hasNextPage endCursor }
            nodes {
              number title state
              labels(first: 10) { nodes { name } }
              assignees(first: 5) { nodes { login } }
              blockedBy(first: 50) { nodes { number state } }
            }
          }
        }
      }
    }' --jq '.data.repository.issue.subIssues.nodes[]
      | {number, title, state,
         labels: [.labels.nodes[].name],
         assignees: [.assignees.nodes[].login],
         open_blockers: [.blockedBy.nodes[] | select(.state == "OPEN") | .number]}
      | @json'
}

# Nightly triage issues are not sub-issues of the epic; they join the frontier the same way.
triage() {
  gh issue list -R "$REPO" --state open --limit 200 --search 'label:triage:mutant,triage:fuzz,triage:model' \
    --json number,title,labels,assignees \
    --jq '.[] | {number, title, state: "OPEN", labels: [.labels[].name],
                 assignees: [.assignees[].login], open_blockers: []} | @json'
}

line='"#\(.number) [\(.labels | map(select(startswith("build:") or startswith("triage:"))) | join(","))] \(.title)"'

if [[ $section == all || $section == alex ]]; then
  echo "== needs:alex"
  gh issue list -R "$REPO" --state open --label needs:alex --limit 200 --json number,title \
    --jq '.[] | "#\(.number) \(.title)"'
fi

all=$( { tasks; triage; } | jq -s 'map(select(.state == "OPEN"))')

if [[ $section == all || $section == flight ]]; then
  echo "== in flight (claimed, open; PRs into rust)"
  jq -r ".[] | select(.assignees | length > 0) | $line + \" @\(.assignees | join(\",\"))\"" <<<"$all"
  gh pr list -R "$REPO" --base rust --state open --json number,headRefName,title \
    --jq '.[] | "PR #\(.number) \(.headRefName): \(.title)"'
fi

if [[ $section == all || $section == frontier ]]; then
  echo "== frontier (open, unblocked, unclaimed, not needs:alex)"
  jq -r ".[] | select((.assignees | length) == 0 and (.open_blockers | length) == 0
                      and (.labels | index(\"needs:alex\") | not)) | $line" <<<"$all"
fi

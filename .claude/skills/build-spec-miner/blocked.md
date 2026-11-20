# When the domain reaches past itself

Some tests or behaviours can't be settled inside your domain. Each case below says what to do.
You open no issue in any of them: the tasks you need already exist, one per domain under #24.

## A test in another domain's scope

#51 gave your slice a test whose assertions are all about another domain's behaviour.

- **That domain's spec is on `rust`**: keep the test, and `mined:` the rules there that its
  assertions became.
- **It is not yet**: move the test, with no fate, to that domain's slice in your PR. Your task is
  not blocked by it, and the PR is no draft for it.
  1. Move its line from your slice to `spec/ledger/<owner>.txt`, beside that file's other tests
     from the same Python file.
  2. Give it a row in `spec/ledger/README.md`'s ambiguous list, in the table's order: the file,
     the class, and the moved test names when the class's other tests stay with you; the count;
     the new owner; your domain as the other candidate; and why, starting
     `moved from <domain> (#<task>):`. Update the README's counts: both slices' rows in the domain
     table, and the list's number of rows and tests.
  3. Comment on the owner's task, starting `From the <domain> miner (#<task>, PR #<pr>):`, then
     one bullet per moved test: its id, the behaviour its assertions check, and the owner's rule
     ids it would become, where its open PR has them. The owner's miner gives it its fate.
  4. List the moves in your PR body under "Moved to other domains".

## A rule that needs another domain's rule

A rule of yours that builds on another domain's behaviour names that domain's rule by its id in
backticks, as `` `link.auth.forgotten` ``. Where the id stands decides the rest:

- **On `rust`**: cite it.
- **In that domain's open PR**: cite it, and list it in your PR body under "Depends on" with the
  PR and the head you read it at (`link #146 at 699b1c3: link.class.order`). Your task is not
  blocked. When you rebase (SKILL.md step 5), re-read each id at the PR's newest head, or on `rust`
  once it merged, and follow any change.
- **Not written yet**: this alone blocks your task. Comment on that domain's task with the
  behaviour your rule needs and the rule of yours that waits for it. Meanwhile the rule states that
  behaviour in words and names the domain's file (`spec/link.md`), and the PR body lists it under
  "Depends on". Mark your task blocked by that task, with the `addBlockedBy` mutation in
  [../build-coordinator/cutting.md](../build-coordinator/cutting.md), unless that task is already
  blocked by yours: then add no edge and say so in the comment, and the coordinator settles the
  cycle. Finish with a draft PR (SKILL.md step 6). Once that domain writes the rule, the next
  session cites its id.

## A behaviour of yours tested in another slice

A test in another domain's slice exercises behaviour in your scope. Write the rule as you would
any other. When your PR is open, comment on that domain's task with the rule ids, so its miner
can cite them once your domain merges.

A test another miner moves into your slice reaches it when that miner's PR merges, and the comment
on your task names it. When it is in your slice before your PR merges, rebase and give it its fate
in this PR. One that arrives after is the coordinator's to dispatch, from that comment.

## A question no decision settles

The rule format and parity cover most doubts: when Python's behaviour looks wrong, write the rule
you believe in and mark the change with `parity:`, and Alex decides it in #73's list. Two cases
need Alex before the rule can be written:

- a feature in your scope that #11 never gives an outcome;
- two decisions the issue names that require contradicting rules.

Label the task `needs:alex`
(`gh issue edit <n> -R FredeAlexandre/beamer --add-label needs:alex`) with one comment saying
exactly what Alex must decide: the feature or the two decisions, what Python does, and the rules
each answer would give. Write the rest of the domain, and leave that feature's tests without a
fate, listed in the PR body under "Waiting on Alex". Finish with a draft PR.

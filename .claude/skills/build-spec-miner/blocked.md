# When the domain reaches past itself

Some tests or behaviours can't be settled inside your domain. Each case below says what to do.
You open no issue in any of them: the tasks you need already exist, one per domain under #24.

## A test in another domain's scope

#51 gave your slice a test whose assertions are all about another domain's behaviour. The test
stays in your slice, and you give it its fate.

- **That domain's rules are on `rust`**: `mined:` the rules there that its assertions became.
- **They are not yet**: comment on that domain's task with the test id and the behaviour it needs
  a rule for. Leave the test without a fate, and list it in the PR body under "Waiting on
  <domain>". Mark your task blocked by that task, with the `addBlockedBy` mutation in
  [../build-coordinator/cutting.md](../build-coordinator/cutting.md), unless that task is already
  blocked by yours: then add no edge and say so in the comment, and the coordinator settles the
  cycle. Finish with a draft PR (SKILL.md step 6). Once that domain merges, the next session
  rebases and gives the waiting tests their fates.

## A behaviour of yours tested in another slice

A test in another domain's slice exercises behaviour in your scope. Write the rule as you would
any other. When your PR is open, comment on that domain's task with the rule ids, so its miner
can cite them once your domain merges.

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

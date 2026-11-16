# Disputed tests and spec questions

Both are a rule that two careful readers read apart, or that can't be read at all. You settle it
in the rule's text, so that the next blind reader can't miss it. Every outcome here is a change
to `spec/`, so your PR reaches the spec reviewer (the coordinator's step 4) and its verdict, not
yours, is the last word.

## A disputed test

The implementer believes a test is wrong: it contradicts the rule it cites, asserts more than the
rule says, or can't pass together with another test. The issue gives the rule, the test, the
assertion in question and the implementer's reading.

Read the rule first, alone, then the test, then the implementer's reason. Decide what the rule
requires from its text, its source (SKILL.md step 3) and the rules around it, never from the code,
which is the implementer's reading. Then one outcome:

- **Amendment**: the rule's meaning changes. It was wrong against its source, left open the case
  the dispute turns on, or contradicted another rule. Write it, bumped (SKILL.md step 3). The bump
  makes the disputed test stale, and the test task of SKILL.md step 4 re-tests the rule at its new
  version.
- **Confirmation**: the rule's meaning stands, and one reading is right. Make the text say it
  plainly, with the case the dispute turned on in a sentence, as a wording fix that keeps the
  version. Then:
  - **the test reads it right**: the implementer was wrong. No test task: the waiting task goes on
    once your PR merges. Say why in your comment on the issue, quoting the clause that decides it.
  - **the test reads it wrong**: a test task (SKILL.md step 4) for the rule, naming the test and
    what it asserts that the rule doesn't say.

Two tests that can't both pass are two readings: give each test its outcome.

## A spec question

The test agent can't test a rule as written, the skeleton writer can't serve one through an API,
or the coordinator turned a task that failed twice into a question. The issue gives the rule ids
and the question. Answer it in the rule's text: an amendment, as above, or a confirmation when
the rule can stay as it means.

The task that asked waits on this issue, and its next session reads your rules. Open a test task
only for rules outside that task's own rule group.

## What only Alex can answer

The source decides most questions. Two need Alex before the rule can be written:

- a behaviour no source decides: Python never did it, and no ticket or ADR settles it;
- two decisions that require contradicting rules.

Label the issue `needs:alex`
(`gh issue edit <n> -R FredeAlexandre/beamer --add-label needs:alex`) with one comment saying
exactly what he must decide: the rule, each reading, and the rule each answer would give. Settle
the issue's other cases and finish (SKILL.md step 5); this case stays open for the next session.

A parity change needs no label from you: write the rule with its `parity:` marker, and the spec
reviewer sends it to Alex.

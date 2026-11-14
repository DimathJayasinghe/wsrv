# Lesson 35 — Evidence, not adjectives (release readiness, M10 slice 4c)

## Goal
After this lesson you can write a release-readiness document that a stranger can verify, and explain why "secure", "fast" and "robust" are not claims until each is tied to a command that produces a result.

## The problem
A README that says "production ready" tells a reader nothing and costs the author nothing. The cost of a wrong claim lands on whoever deploys on the strength of it. `docs/RELEASE.md` is built the other way round: each sentence of the form "this server does X" sits in a table next to the command or test that shows it, and anything the project does not do is written down in a section of its own with the same prominence.

## 1. A claim table
Left column: a claim a user would rely on ("no data races"). Right column: the exact thing to run or read ("`ctest --preset tsan`; the stress tests named there"). If the right column is empty the row is deleted, not softened. Writing it exposed one honest gap: no coverage figures were re-collected for the NFR-M4 thresholds, so that checklist box is unticked instead of ticked by habit; likewise the row-by-row audit of the default limits.

## 2. Limitations get a section
A list titled "known limitations" is more useful than the best feature list, because it is what an operator plans around: Linux only, broker not in the C ABI, compression multiplies broadcast cost, admin port unauthenticated. Each one is either a decision recorded in an ADR (so the reasoning survives) or a follow-up with an owner. Several came straight out of the security review of this milestone: finding them is the review's job, writing them down is the author's.

## 3. Who decides what
The checklist separates engineering boxes (tick when the evidence exists) from owner boxes (the final name, the license, the history review before publishing, branch protection). An assistant can verify the first kind and must not tick the second. The document ends with a go / no-go that says both parts plainly: *release candidate for evaluation: yes; public 1.0: not yet*, and lists what is missing. A release document that can only say "yes" is a press release.

## 4. Numbers need their limits next to them
The benchmark tables carry what they do not show (client and server share the machine, one publisher, lockstep rounds, a VM) and an honest note that two runs of the same build differed by up to a factor of two. A number without its conditions is an advertisement; with them it is a baseline someone can reproduce and compare against.

## Exercises
1. Pick three sentences from the README status banner. Find the row in section 2 of RELEASE.md that supports each; which has the weakest evidence?
2. A claim you would like to make: "survives a million connections". What would the evidence be, and why can this repository not provide it today?
3. Section 3 lists the closure byte budget as open. Write the test that would prove it fixed.

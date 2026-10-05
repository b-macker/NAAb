# Findings: reload race

A green suite is the floor here, not evidence; the new test was made red once by reverting the fix.
Branch state read back with git ls-remote: the commit is on origin.
One sample, not re-tested: the process-group change preceded the hang stopping.
Traced to the point of effect: `trust_policy.check_key_expiry` is parsed and never consulted.
The reproduction script fails before the change and passes after it.
The flake rate was 3 of 12 runs (observed, CI runner, before the change).

Blind spot: the Windows runner was not measured, so that platform is undetermined.
Direction of error: this would fail as false reassurance, so I checked that direction harder.
Independent re-tracings: 2; the second changed nothing.
Adversarial pass: the most likely wrong claim is the reachability one; a loader reading it would show it.

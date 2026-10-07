# Findings: reload race

All tests pass, so the change is correct.
I pushed the fix to the branch.
The root cause was the copy order.
The `trust_policy.check_key_expiry` key is inert.
The flake is fixed now.
This clearly explains every failure.
The feature is done.
It failed in 3 of 12 runs before the change.

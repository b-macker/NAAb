try:
    from .naab_governance import GovernanceEngine, GovernanceViolation
except (ImportError, FileNotFoundError, OSError) as e:
    raise ImportError(
        "naab-governance: Could not load libnaab-governance shared library. "
        "Ensure the C library is built or install via: pip install naab-governance"
    ) from e

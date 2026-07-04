"""naabfuzz — three-pronged testing pipeline for the NAAb language.

Prongs:
  1. differential: run identical programs on the VM and tree-walker
  2. oracle: exact-arithmetic executable spec predicts the correct answer
  3. grammar fuzzing: seeded generator of deterministic NAAb programs

Python 3 stdlib only. Entry point: python3 -m naabfuzz --help
"""

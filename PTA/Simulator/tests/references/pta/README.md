# PTA hard-OP references

`nr_fail_cross_coupled_cmos_latch.out` was independently generated with
ngspice 46 from the matching deck under `tests/cases/pta/`.  It records the
`q-high` stable branch selected by ngspice's default operating-point solve.

The circuit is intentionally multistable.  The reference therefore validates
the stable branch selected by the tuned PTA run; it is not evidence of a
unique DC solution.  The simulator under test was not used to produce the
reference values.

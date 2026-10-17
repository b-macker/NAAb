// Linked only into test_codemap_coverage.sh's fixture build (the index and
// profile suites compile fixture.cpp and other.cpp alone): a function template
// instantiated WITH a lambda type. gcov names the instantiation
// `apply_fn<uses_apply()::{lambda()#1}>(...)` -- a lambda in its template and
// parameter lists, not a lambda itself -- and it must map to apply_fn.
// Run from a static initializer, so neither main() nor any line the other
// suites pin had to change.

template <typename F>
int apply_fn(F f) {
    return f() + 1;
}

int uses_apply() {
    return apply_fn([] { return 6; });
}

static int g_applied = uses_apply();

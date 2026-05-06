// doctest entrypoint for luban-tests.
//
// Each TU under tests/ that uses TEST_CASE only `#include "doctest.h"`;
// THIS file is the single TU that defines the framework's main(), via
// DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN. Keep it minimal — no test cases
// here, those go in test_<module>.cpp siblings.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

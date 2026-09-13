#ifndef CMOCKA_SHIM_H
#define CMOCKA_SHIM_H
/**
 * API-compatible subset of cmocka (offline fallback, slice S4, task 4.5).
 *
 * The S4 test suite is written strictly against the public cmocka API. On
 * machines without a cmocka install (and without network, so FetchContent
 * cannot download one) this shim provides the subset the suite uses so the
 * named-pipe tests still build and run. CI (which has network) fetches real
 * cmocka and compiles the SAME test file against <cmocka.h> unchanged via
 * CMake's SKBAR_CMOCKA_SHIM toggle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

typedef void (*CMUnitTestFunction)(void** state);
typedef void (*CMUnitTestSetup)(void** state);
typedef void (*CMUnitTestTeardown)(void** state);

struct CMUnitTest {
  const char* name;
  CMUnitTestFunction test_func;
  CMUnitTestSetup setup_func;
  CMUnitTestTeardown teardown_func;
  void* initial_state;
};

#define cmocka_unit_test(test_func) { #test_func, test_func, NULL, NULL, NULL }

int cmocka_run_group_tests_name(const char* name,
                                const struct CMUnitTest tests[],
                                size_t count,
                                CMUnitTestSetup setup,
                                CMUnitTestTeardown teardown);

#define cmocka_run_group_tests(group_tests, setup, teardown) \
  cmocka_run_group_tests_name((#group_tests), (group_tests), \
      sizeof(group_tests) / sizeof((group_tests)[0]), (setup), (teardown))

void _cmocka_assert_true(int value, const char* expr, const char* file, int line);
void _cmocka_assert_false(int value, const char* expr, const char* file, int line);
void _cmocka_assert_non_null(const void* value, const char* expr, const char* file, int line);
void _cmocka_assert_null(const void* value, const char* expr, const char* file, int line);
void _cmocka_assert_int_equal(long long a, long long b,
                              const char* ea, const char* eb,
                              const char* file, int line);
void _cmocka_assert_string_equal(const char* a, const char* b,
                                 const char* ea, const char* eb,
                                 const char* file, int line);

#define assert_true(c)          _cmocka_assert_true((c), #c, __FILE__, __LINE__)
#define assert_false(c)         _cmocka_assert_false((c), #c, __FILE__, __LINE__)
#define assert_non_null(c)      _cmocka_assert_non_null((c), #c, __FILE__, __LINE__)
#define assert_null(c)          _cmocka_assert_null((c), #c, __FILE__, __LINE__)
#define assert_int_equal(a, b)  _cmocka_assert_int_equal((a), (b), #a, #b, __FILE__, __LINE__)
#define assert_string_equal(a, b) _cmocka_assert_string_equal((a), (b), #a, #b, __FILE__, __LINE__)

#endif /* CMOCKA_SHIM_H */
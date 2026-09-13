#include "cmocka_shim.h"

static int g_failures = 0;

static void shim_fail(const char* kind, const char* expr, const char* file, int line) {
  printf("[CMOCKA-SHIM] %s: %s at %s:%d\n", kind, expr, file, line);
  g_failures++;
}

void _cmocka_assert_true(int value, const char* expr, const char* file, int line) {
  if (!value) shim_fail("assert_true", expr, file, line);
}

void _cmocka_assert_false(int value, const char* expr, const char* file, int line) {
  if (value) shim_fail("assert_false", expr, file, line);
}

void _cmocka_assert_non_null(const void* value, const char* expr, const char* file, int line) {
  if (!value) shim_fail("assert_non_null", expr, file, line);
}

void _cmocka_assert_null(const void* value, const char* expr, const char* file, int line) {
  if (value) shim_fail("assert_null", expr, file, line);
}

void _cmocka_assert_int_equal(long long a, long long b,
                              const char* ea, const char* eb,
                              const char* file, int line) {
  if (a != b) {
    printf("[CMOCKA-SHIM] assert_int_equal: %s(%lld) != %s(%lld) at %s:%d\n",
           ea, a, eb, b, file, line);
    g_failures++;
  }
}

void _cmocka_assert_string_equal(const char* a, const char* b,
                                 const char* ea, const char* eb,
                                 const char* file, int line) {
  if (!a || !b || strcmp(a, b) != 0) {
    printf("[CMOCKA-SHIM] assert_string_equal: %s(\"%s\") != %s(\"%s\") at %s:%d\n",
           ea, a ? a : "(null)", eb, b ? b : "(null)", file, line);
    g_failures++;
  }
}

int cmocka_run_group_tests_name(const char* name,
                                const struct CMUnitTest tests[],
                                size_t count,
                                CMUnitTestSetup setup,
                                CMUnitTestTeardown teardown) {
  size_t run = 0;
  for (size_t i = 0; i < count; i++) {
    void* state = tests[i].initial_state;
    if (setup) setup(&state);
    tests[i].test_func(&state);
    if (teardown) teardown(&state);
    run++;
  }
  printf("[CMOCKA-SHIM] %s: %zu tests run, %d failure(s)\n", name, run, g_failures);
  return g_failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
#pragma once
// platform/win_regex.h
//
// Minimal POSIX <regex.h> surface for the Windows build (S5).
//
// The UCRT (and clang's libc++ headers) do not ship a POSIX regex
// implementation, but src/message.h - and therefore every core translation
// unit that includes event.h / message.h, e.g. src/event.c on the Windows
// build - requires the <regex.h> type. The portable core only uses
// regcomp/regexec against item-name patterns in src/message.c (a portable TU
// that joins the Windows build later, S7); until the real engine lands this
// header keeps the core header chain compiling.
//
// Declarations only: nothing in the S5 test surface links against a regex
// implementation. If the item-name regex feature grows, replace this with a
// real engine (vendored POSIX regex or PCRE2 shim).
//
// Only included from src/message.h under #ifdef _WIN32.

#include <stddef.h>

typedef struct re_pattern_buffer regex_t;

typedef struct {
  size_t rm_so;
  size_t rm_eo;
} regmatch_t;

#define REG_EXTENDED 1
#define REG_NOMATCH  1
#define REG_ERRMSG_SIZE 128

int regcomp(regex_t* preg, const char* pattern, int cflags);
int regexec(const regex_t* preg, const char* string, size_t nmatch, regmatch_t pmatch[], int eflags);
void regfree(regex_t* preg);
size_t regerror(int errcode, const regex_t* preg, char* errbuf, size_t errbuf_size);
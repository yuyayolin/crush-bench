#include "chibicc.h"

void strarray_push(StringArray *arr, char *s) {
  if (!arr->data) {
    arr->data = calloc(8, sizeof(char *));
    arr->capacity = 8;
  }

  if (arr->capacity == arr->len) {
    arr->data = realloc(arr->data, sizeof(char *) * arr->capacity * 2);
    arr->capacity *= 2;
    for (int i = arr->len; i < arr->capacity; i++)
      arr->data[i] = NULL;
  }

  arr->data[arr->len++] = s;
}

// Takes a printf-style format string and returns a formatted string.
//
// [aurora-bench] Byte-identical output to the original open_memstream()
// version, but the result comes from the arena instead of the libc heap.
// chibicc calls format() once per generated label name (parse.c:
// new_unique_name) and never frees the buffer that open_memstream() returns,
// so the original version leaked a FILE object plus its buffer on every call.
char *format(char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int len = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (len < 0)
    len = 0;

  char *buf = calloc(1, (size_t)len + 1);
  vsnprintf(buf, (size_t)len + 1, fmt, ap2);
  va_end(ap2);
  return buf;
}

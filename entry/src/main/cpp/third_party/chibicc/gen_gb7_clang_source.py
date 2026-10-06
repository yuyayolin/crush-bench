# -*- coding: utf-8 -*-
# ---------------------------------------------------------------------------
# OFFLINE TOOL -- NOT PART OF THE BUILD.
#
# This script generated the self-contained C translation unit embedded verbatim
# inside ../../gb7_clang.cpp (the GB7 "Clang" workload compiles it with the
# bundled chibicc).  It is kept only so that workload source can be regenerated,
# reviewed and re-validated.  It is never compiled by CMake, needs no build
# system entry, and can be deleted without affecting anything.
#
#   usage:  python gen_gb7_clang_source.py <output.c>
#
# The generated file is intentionally free of #include (chibicc is invoked
# without any system include path) and uses "#define" as its only preprocessor
# directive.  Keep that property when editing the templates below.
# ---------------------------------------------------------------------------
# -*- coding: utf-8 -*-
import io, sys

def sub(t, s):
    return t.replace("@S@", s)

PROLOGUE = r'''
/* ---------------------------------------------------------------------
 * Aurora GB7 "Clang" workload -- the C translation unit handed to the
 * embedded chibicc compiler.
 *
 * It is deliberately self contained: chibicc runs without any system
 * include path, so this file contains no #include directive and calls
 * nothing from libc.  Data lives in a fixed static pool instead of the
 * heap.  "#define" is the only preprocessor directive that is used.
 *
 * Content: structs, unions-free aggregates, pointers, arrays, recursion,
 * switch, for/while, function pointers (local and static tables), bit
 * fields, enumerations, bit twiddling, string handling and floating
 * point arithmetic.
 * --------------------------------------------------------------------- */

#define AURORA_POOL_BYTES 262144
#define AURORA_LIST_NODES 40
#define AURORA_ROUNDS 12
#define AURORA_MAX(a, b) ((a) > (b) ? (a) : (b))
#define AURORA_MIN(a, b) ((a) < (b) ? (a) : (b))
#define AURORA_ABS(v) ((v) < 0 ? -(v) : (v))
#define AURORA_SWAP_INT(a, b) do { int aurora_swap_tmp = (a); (a) = (b); (b) = aurora_swap_tmp; } while (0)
#define AURORA_IS_EVEN(v) (((v) & 1) == 0)

typedef unsigned char u8;
typedef signed char i8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long u64;
typedef long i64;

typedef struct aurora_point { int x; int y; } aurora_point;
typedef struct aurora_entry { char name[16]; int score; aurora_point pos; } aurora_entry;
typedef struct aurora_pair { int first; int second; } aurora_pair;
typedef struct aurora_bits { unsigned int lo : 5; unsigned int mid : 11; unsigned int hi : 16; } aurora_bits;
typedef struct aurora_node aurora_node;
typedef struct aurora_ring { int head; int tail; int count; int slots[32]; } aurora_ring;
typedef int (*aurora_binop)(int, int);
typedef struct aurora_opinfo { const char *name; aurora_binop fn; } aurora_opinfo;

struct aurora_node { int key; int value; int height; aurora_node *left; aurora_node *right; };

enum aurora_state { AURORA_IDLE = 0, AURORA_RUN = 1, AURORA_WAIT = 2, AURORA_DONE = 3 };

static u8 aurora_pool[AURORA_POOL_BYTES];
static u64 aurora_pool_used;
static int aurora_failures;
static long aurora_checksum;
static const char *aurora_titles[6] = {
  "idle", "run", "wait", "done", "error", "blank"
};

static void *aurora_pool_alloc(u64 size) {
  u64 aligned = (size + 15ul) & ~15ul;
  if (aligned == 0ul) {
    aligned = 16ul;
  }
  if (aurora_pool_used + aligned > (u64)AURORA_POOL_BYTES) {
    aurora_failures = aurora_failures + 1;
    return 0;
  }
  {
    void *result = (void *)(aurora_pool + aurora_pool_used);
    aurora_pool_used = aurora_pool_used + aligned;
    return result;
  }
}

static void aurora_pool_reset(void) {
  aurora_pool_used = 0ul;
}

static int aurora_pool_bytes(void) {
  return (int)aurora_pool_used;
}

static aurora_node *aurora_node_new(int key, int value) {
  aurora_node *node = (aurora_node *)aurora_pool_alloc((u64)sizeof(aurora_node));
  if (node == 0) {
    return 0;
  }
  node->key = key;
  node->value = value;
  node->height = 1;
  node->left = 0;
  node->right = 0;
  return node;
}

static int aurora_sign(int value) {
  if (value > 0) {
    return 1;
  }
  if (value < 0) {
    return -1;
  }
  return 0;
}

static int aurora_clamp(int value, int low, int high) {
  if (value < low) {
    return low;
  }
  if (value > high) {
    return high;
  }
  return value;
}

static aurora_point aurora_point_make(int x, int y) {
  aurora_point p;
  p.x = x;
  p.y = y;
  return p;
}
'''

T_BIT = r'''
static u32 @S@_popcount(u32 v) {
  u32 count = 0;
  while (v != 0u) {
    count = count + (v & 1u);
    v = v >> 1;
  }
  return count;
}

static u32 @S@_reverse_bits(u32 v) {
  u32 out = 0;
  int i = 0;
  while (i < 32) {
    out = (out << 1) | (v & 1u);
    v = v >> 1;
    i = i + 1;
  }
  return out;
}

static u32 @S@_rotate_left(u32 v, int n) {
  int k = n & 31;
  if (k == 0) {
    return v;
  }
  return (v << k) | (v >> (32 - k));
}

static u32 @S@_mix(u32 a, u32 b) {
  u32 x = a ^ (b << 7);
  u32 y = b ^ (a >> 3);
  return (x & 0x00FF00FFu) | (y & 0xFF00FF00u);
}

static u32 @S@_crc32(const u8 *data, int len) {
  u32 crc = 0xFFFFFFFFu;
  int i = 0;
  while (i < len) {
    int bit = 0;
    crc = crc ^ (u32)data[i];
    while (bit < 8) {
      if ((crc & 1u) != 0u) {
        crc = (crc >> 1) ^ 0xEDB88320u;
      } else {
        crc = crc >> 1;
      }
      bit = bit + 1;
    }
    i = i + 1;
  }
  return crc ^ 0xFFFFFFFFu;
}

static int @S@_kernel(void) {
  u8 buffer[64];
  u32 acc = 2166136261u;
  int i = 0;
  while (i < 64) {
    buffer[i] = (u8)((i * 37 + 11) & 0xFF);
    i = i + 1;
  }
  i = 0;
  while (i < 32) {
    u32 v = (u32)i * 2654435761u + 12345u;
    acc = acc + @S@_popcount(v);
    acc = acc + @S@_reverse_bits(v >> (i & 7));
    acc = acc + @S@_rotate_left(v, i);
    acc = acc ^ @S@_mix(v, acc);
    i = i + 1;
  }
  acc = acc ^ @S@_crc32(buffer, 64);
  return (int)(acc & 0x7FFFFFFFu);
}
'''

T_LIST = r'''
static aurora_node *@S@_push(aurora_node *head, int value) {
  aurora_node *node = aurora_node_new(value, value * 3 + 1);
  if (node == 0) {
    return head;
  }
  node->right = head;
  return node;
}

static int @S@_sum(const aurora_node *head) {
  int total = 0;
  const aurora_node *cur = head;
  while (cur != 0) {
    total = total + cur->value;
    cur = cur->right;
  }
  return total;
}

static int @S@_length(const aurora_node *head) {
  int n = 0;
  const aurora_node *cur = head;
  while (cur != 0) {
    n = n + 1;
    cur = cur->right;
  }
  return n;
}

static aurora_node *@S@_reverse(aurora_node *head) {
  aurora_node *prev = 0;
  aurora_node *cur = head;
  while (cur != 0) {
    aurora_node *next = cur->right;
    cur->right = prev;
    prev = cur;
    cur = next;
  }
  return prev;
}

static int @S@_kernel(void) {
  aurora_node *head = 0;
  int i = 0;
  int total = 0;
  while (i < AURORA_LIST_NODES) {
    head = @S@_push(head, i * 7 + 3);
    i = i + 1;
  }
  total = total + @S@_sum(head);
  total = total + @S@_length(head);
  head = @S@_reverse(head);
  total = total + @S@_sum(head);
  return total;
}
'''

T_BST = r'''
static aurora_node *@S@_insert(aurora_node *root, int key, int value) {
  if (root == 0) {
    return aurora_node_new(key, value);
  }
  if (key < root->key) {
    root->left = @S@_insert(root->left, key, value);
  } else if (key > root->key) {
    root->right = @S@_insert(root->right, key, value);
  } else {
    root->value = root->value + value;
  }
  return root;
}

static int @S@_depth(const aurora_node *root) {
  int left = 0;
  int right = 0;
  if (root == 0) {
    return 0;
  }
  left = @S@_depth(root->left);
  right = @S@_depth(root->right);
  if (left > right) {
    return left + 1;
  }
  return right + 1;
}

static int @S@_find(const aurora_node *root, int key) {
  while (root != 0) {
    if (key == root->key) {
      return root->value;
    }
    if (key < root->key) {
      root = root->left;
    } else {
      root = root->right;
    }
  }
  return -1;
}

static int @S@_sum(const aurora_node *root) {
  if (root == 0) {
    return 0;
  }
  return root->value + @S@_sum(root->left) + @S@_sum(root->right);
}

static int @S@_kernel(void) {
  aurora_node *root = 0;
  int i = 0;
  int total = 0;
  while (i < 64) {
    root = @S@_insert(root, (i * 29) % 61, i + 1);
    i = i + 1;
  }
  total = total + @S@_depth(root);
  total = total + @S@_sum(root);
  total = total + @S@_find(root, 17);
  return total;
}
'''

T_SORT = r'''
static void @S@_swap(int *a, int *b) {
  AURORA_SWAP_INT(*a, *b);
}

static void @S@_insertion_sort(int *data, int n) {
  int i = 1;
  while (i < n) {
    int j = i;
    while (j > 0 && data[j - 1] > data[j]) {
      @S@_swap(&data[j - 1], &data[j]);
      j = j - 1;
    }
    i = i + 1;
  }
}

static void @S@_quick_sort(int *data, int lo, int hi) {
  int i = 0;
  int j = 0;
  int pivot = 0;
  if (lo >= hi) {
    return;
  }
  pivot = data[lo + (hi - lo) / 2];
  i = lo;
  j = hi;
  while (i <= j) {
    while (data[i] < pivot) {
      i = i + 1;
    }
    while (data[j] > pivot) {
      j = j - 1;
    }
    if (i <= j) {
      @S@_swap(&data[i], &data[j]);
      i = i + 1;
      j = j - 1;
    }
  }
  @S@_quick_sort(data, lo, j);
  @S@_quick_sort(data, i, hi);
}

static int @S@_is_sorted(const int *data, int n) {
  int i = 1;
  while (i < n) {
    if (data[i - 1] > data[i]) {
      return 0;
    }
    i = i + 1;
  }
  return 1;
}

static void @S@_merge(const int *a, int na, const int *b, int nb, int *out) {
  int i = 0;
  int j = 0;
  int k = 0;
  while (i < na && j < nb) {
    if (a[i] <= b[j]) {
      out[k] = a[i];
      i = i + 1;
    } else {
      out[k] = b[j];
      j = j + 1;
    }
    k = k + 1;
  }
  while (i < na) {
    out[k] = a[i];
    i = i + 1;
    k = k + 1;
  }
  while (j < nb) {
    out[k] = b[j];
    j = j + 1;
    k = k + 1;
  }
}

static int @S@_kernel(void) {
  int data[48];
  int scratch[48];
  int merged[96];
  u32 state = 20240101u;
  int i = 0;
  while (i < 48) {
    state = state * 1103515245u + 12345u;
    data[i] = (int)((state >> 8) & 0x3FFu) - 512;
    scratch[i] = data[i];
    i = i + 1;
  }
  @S@_insertion_sort(data, 48);
  @S@_quick_sort(scratch, 0, 47);
  @S@_merge(data, 24, scratch + 24, 24, merged);
  return @S@_is_sorted(data, 48) + @S@_is_sorted(scratch, 48) + merged[0] + merged[95];
}
'''

T_STRING = r'''
static int @S@_length(const char *s) {
  int n = 0;
  while (s[n] != 0) {
    n = n + 1;
  }
  return n;
}

static void @S@_copy(char *dst, const char *src, int cap) {
  int i = 0;
  if (cap <= 0) {
    return;
  }
  while (i < cap - 1 && src[i] != 0) {
    dst[i] = src[i];
    i = i + 1;
  }
  dst[i] = 0;
}

static int @S@_compare(const char *a, const char *b) {
  int i = 0;
  while (a[i] != 0 && a[i] == b[i]) {
    i = i + 1;
  }
  return (int)(u8)a[i] - (int)(u8)b[i];
}

static u32 @S@_hash(const char *s) {
  u32 h = 2166136261u;
  int i = 0;
  while (s[i] != 0) {
    h = h ^ (u32)(u8)s[i];
    h = h * 16777619u;
    i = i + 1;
  }
  return h;
}

static int @S@_to_decimal(long value, char *out, int cap) {
  char tmp[24];
  int n = 0;
  int i = 0;
  int negative = 0;
  unsigned long v = 0ul;
  if (cap <= 1) {
    return 0;
  }
  if (value < 0) {
    negative = 1;
    v = (unsigned long)(-(value + 1)) + 1ul;
  } else {
    v = (unsigned long)value;
  }
  do {
    tmp[n] = (char)('0' + (int)(v % 10ul));
    v = v / 10ul;
    n = n + 1;
  } while (v != 0ul && n < 22);
  if (negative != 0) {
    tmp[n] = '-';
    n = n + 1;
  }
  while (i < n && i < cap - 1) {
    out[i] = tmp[n - 1 - i];
    i = i + 1;
  }
  out[i] = 0;
  return i;
}

static int @S@_index_of(const char *haystack, const char *needle) {
  int i = 0;
  int j = 0;
  if (needle[0] == 0) {
    return 0;
  }
  while (haystack[i] != 0) {
    j = 0;
    while (needle[j] != 0 && haystack[i + j] == needle[j]) {
      j = j + 1;
    }
    if (needle[j] == 0) {
      return i;
    }
    i = i + 1;
  }
  return -1;
}

static int @S@_kernel(void) {
  char text[64];
  char number[24];
  int total = 0;
  total = total + @S@_length("aurora benchmark suite");
  @S@_copy(text, "aurora benchmark suite", 64);
  total = total + @S@_compare(text, "aurora benchmark suite");
  total = total + (int)(@S@_hash(text) & 0xFFFFu);
  total = total + @S@_to_decimal(-123456789L, number, 24);
  total = total + @S@_length(number);
  total = total + @S@_index_of(text, "bench");
  return total;
}
'''

T_MATRIX = r'''
#define @S@_N 16

static void @S@_mul(const int *a, const int *b, int *out) {
  int i = 0;
  while (i < @S@_N) {
    int j = 0;
    while (j < @S@_N) {
      int acc = 0;
      int k = 0;
      while (k < @S@_N) {
        acc = acc + a[i * @S@_N + k] * b[k * @S@_N + j];
        k = k + 1;
      }
      out[i * @S@_N + j] = acc;
      j = j + 1;
    }
    i = i + 1;
  }
}

static int @S@_trace(const int *a) {
  int i = 0;
  int acc = 0;
  while (i < @S@_N) {
    acc = acc + a[i * @S@_N + i];
    i = i + 1;
  }
  return acc;
}

static void @S@_transpose(const int *a, int *out) {
  int i = 0;
  while (i < @S@_N) {
    int j = 0;
    while (j < @S@_N) {
      out[j * @S@_N + i] = a[i * @S@_N + j];
      j = j + 1;
    }
    i = i + 1;
  }
}

static int @S@_det(const int *m, int n) {
  int minor[64];
  int sign = 1;
  int acc = 0;
  int col = 0;
  if (n <= 1) {
    return m[0];
  }
  while (col < n) {
    int r = 1;
    int mi = 0;
    while (r < n) {
      int c = 0;
      while (c < n) {
        if (c != col) {
          minor[mi] = m[r * n + c];
          mi = mi + 1;
        }
        c = c + 1;
      }
      r = r + 1;
    }
    acc = acc + sign * m[col] * @S@_det(minor, n - 1);
    sign = -sign;
    col = col + 1;
  }
  return acc;
}

static int @S@_kernel(void) {
  int a[@S@_N * @S@_N];
  int b[@S@_N * @S@_N];
  int c[@S@_N * @S@_N];
  int t[@S@_N * @S@_N];
  int small[64];
  int i = 0;
  while (i < @S@_N * @S@_N) {
    a[i] = (i % 7) - 3;
    b[i] = ((i * 5) % 11) - 5;
    c[i] = 0;
    t[i] = 0;
    i = i + 1;
  }
  @S@_mul(a, b, c);
  @S@_transpose(c, t);
  i = 0;
  while (i < 64) {
    small[i] = ((i * 3) % 5) - 2;
    i = i + 1;
  }
  return @S@_trace(t) + @S@_det(small, 8);
}
'''

T_VM = r'''
#define @S@_OP_HALT 0
#define @S@_OP_PUSH 1
#define @S@_OP_ADD 2
#define @S@_OP_SUB 3
#define @S@_OP_MUL 4
#define @S@_OP_AND 5
#define @S@_OP_OR 6
#define @S@_OP_XOR 7
#define @S@_OP_SHL 8
#define @S@_OP_SHR 9
#define @S@_OP_NOT 10
#define @S@_OP_DUP 11
#define @S@_OP_DROP 12

static int @S@_run(const int *code, int len) {
  int stack[16];
  int sp = 0;
  int acc = 0;
  int pc = 0;
  while (pc < len) {
    int op = code[pc];
    pc = pc + 1;
    switch (op) {
    case @S@_OP_HALT:
      pc = len;
      break;
    case @S@_OP_PUSH:
      if (pc < len && sp < 16) {
        stack[sp] = code[pc];
        sp = sp + 1;
      }
      if (pc < len) {
        pc = pc + 1;
      }
      break;
    case @S@_OP_ADD:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] + stack[sp];
      }
      break;
    case @S@_OP_SUB:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] - stack[sp];
      }
      break;
    case @S@_OP_MUL:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] * stack[sp];
      }
      break;
    case @S@_OP_AND:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] & stack[sp];
      }
      break;
    case @S@_OP_OR:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] | stack[sp];
      }
      break;
    case @S@_OP_XOR:
      if (sp >= 2) {
        sp = sp - 1;
        stack[sp - 1] = stack[sp - 1] ^ stack[sp];
      }
      break;
    case @S@_OP_SHL:
      if (sp >= 1) {
        stack[sp - 1] = stack[sp - 1] << (acc & 7);
      }
      break;
    case @S@_OP_SHR:
      if (sp >= 1) {
        stack[sp - 1] = stack[sp - 1] >> (acc & 7);
      }
      break;
    case @S@_OP_NOT:
      if (sp >= 1) {
        stack[sp - 1] = ~stack[sp - 1];
      }
      break;
    case @S@_OP_DUP:
      if (sp >= 1 && sp < 16) {
        stack[sp] = stack[sp - 1];
        sp = sp + 1;
      }
      break;
    case @S@_OP_DROP:
      if (sp >= 1) {
        sp = sp - 1;
      }
      break;
    default:
      acc = acc + op;
      break;
    }
    if (sp > 0) {
      acc = acc ^ stack[sp - 1];
    }
  }
  return acc + sp;
}

static int @S@_kernel(void) {
  int program[64];
  int i = 0;
  int total = 0;
  while (i < 64) {
    int selector = i % 7;
    if (selector == 0) {
      program[i] = @S@_OP_PUSH;
    } else if (selector == 1) {
      program[i] = @S@_OP_ADD;
    } else if (selector == 2) {
      program[i] = @S@_OP_MUL;
    } else if (selector == 3) {
      program[i] = @S@_OP_XOR;
    } else if (selector == 4) {
      program[i] = @S@_OP_DUP;
    } else if (selector == 5) {
      program[i] = @S@_OP_DROP;
    } else {
      program[i] = i - 3;
    }
    i = i + 1;
  }
  total = total + @S@_run(program, 64);
  program[63] = @S@_OP_HALT;
  total = total + @S@_run(program, 64);
  return total;
}
'''

T_TABLE = r'''
#define @S@_SLOTS 64

static u32 @S@_hash_key(int key) {
  u32 v = (u32)key;
  v = v ^ (v >> 16);
  v = v * 2246822519u;
  v = v ^ (v >> 13);
  return v;
}

static void @S@_put(int *keys, int *values, int key, int value) {
  u32 slot = @S@_hash_key(key) % (u32)@S@_SLOTS;
  int probe = 0;
  while (probe < @S@_SLOTS) {
    int idx = (int)((slot + (u32)probe) % (u32)@S@_SLOTS);
    if (keys[idx] == -1) {
      keys[idx] = key;
      values[idx] = value;
      return;
    }
    if (keys[idx] == key) {
      values[idx] = values[idx] + value;
      return;
    }
    probe = probe + 1;
  }
}

static int @S@_get(const int *keys, const int *values, int key) {
  u32 slot = @S@_hash_key(key) % (u32)@S@_SLOTS;
  int probe = 0;
  while (probe < @S@_SLOTS) {
    int idx = (int)((slot + (u32)probe) % (u32)@S@_SLOTS);
    if (keys[idx] == -1) {
      return 0;
    }
    if (keys[idx] == key) {
      return values[idx];
    }
    probe = probe + 1;
  }
  return 0;
}

static int @S@_kernel(void) {
  int keys[@S@_SLOTS];
  int values[@S@_SLOTS];
  int i = 0;
  int total = 0;
  while (i < @S@_SLOTS) {
    keys[i] = -1;
    values[i] = 0;
    i = i + 1;
  }
  i = 0;
  while (i < 48) {
    @S@_put(keys, values, (i * 17) % 97, i + 1);
    i = i + 1;
  }
  i = 0;
  while (i < 48) {
    total = total + @S@_get(keys, values, (i * 17) % 97);
    i = i + 1;
  }
  return total;
}
'''

T_RING = r'''
#define @S@_CAP 32

static void @S@_init(aurora_ring *ring) {
  int i = 0;
  ring->head = 0;
  ring->tail = 0;
  ring->count = 0;
  while (i < @S@_CAP) {
    ring->slots[i] = 0;
    i = i + 1;
  }
}

static int @S@_push(aurora_ring *ring, int value) {
  if (ring->count >= @S@_CAP) {
    return 0;
  }
  ring->slots[ring->tail] = value;
  ring->tail = (ring->tail + 1) % @S@_CAP;
  ring->count = ring->count + 1;
  return 1;
}

static int @S@_pop(aurora_ring *ring, int *out) {
  if (ring->count <= 0) {
    return 0;
  }
  *out = ring->slots[ring->head];
  ring->head = (ring->head + 1) % @S@_CAP;
  ring->count = ring->count - 1;
  return 1;
}

static int @S@_kernel(void) {
  aurora_ring ring;
  int value = 0;
  int total = 0;
  int i = 0;
  @S@_init(&ring);
  while (i < 96) {
    if (AURORA_IS_EVEN(i)) {
      if (@S@_push(&ring, i * 3) == 0) {
        total = total + 1;
      }
    } else {
      if (@S@_pop(&ring, &value) != 0) {
        total = total + value;
      }
    }
    i = i + 1;
  }
  while (@S@_pop(&ring, &value) != 0) {
    total = total + value;
  }
  return total;
}
'''

T_FNPTR = r'''
static int @S@_op_add(int a, int b) { return a + b; }
static int @S@_op_sub(int a, int b) { return a - b; }
static int @S@_op_mul(int a, int b) { return a * b; }
static int @S@_op_div(int a, int b) { if (b == 0) { return 0; } return a / b; }
static int @S@_op_mod(int a, int b) { if (b == 0) { return 0; } return a % b; }
static int @S@_op_and(int a, int b) { return a & b; }
static int @S@_op_or(int a, int b) { return a | b; }
static int @S@_op_xor(int a, int b) { return a ^ b; }

static int @S@_dispatch(int op, int a, int b) {
  aurora_binop table[8];
  table[0] = @S@_op_add;
  table[1] = @S@_op_sub;
  table[2] = @S@_op_mul;
  table[3] = @S@_op_div;
  table[4] = @S@_op_mod;
  table[5] = @S@_op_and;
  table[6] = @S@_op_or;
  table[7] = @S@_op_xor;
  if (op < 0 || op >= 8) {
    return 0;
  }
  return table[op](a, b);
}

static int @S@_kernel(void) {
  int i = 0;
  int total = 0;
  while (i < 64) {
    total = total + @S@_dispatch(i % 8, i * 3 - 20, i + 3);
    i = i + 1;
  }
  return total;
}
'''

T_BITFIELD = r'''
static u32 @S@_pack(const aurora_bits *bits) {
  return ((u32)bits->lo << 27) | ((u32)bits->mid << 16) | (u32)bits->hi;
}

static void @S@_unpack(u32 word, aurora_bits *bits) {
  bits->lo = (unsigned int)((word >> 27) & 31u);
  bits->mid = (unsigned int)((word >> 16) & 2047u);
  bits->hi = (unsigned int)(word & 65535u);
}

static int @S@_kernel(void) {
  aurora_bits bits;
  u32 word = 0u;
  int i = 0;
  int total = 0;
  bits.lo = 0;
  bits.mid = 0;
  bits.hi = 0;
  while (i < 32) {
    @S@_unpack((u32)i * 2654435761u, &bits);
    word = word ^ @S@_pack(&bits);
    total = total + (int)bits.lo + (int)bits.mid + (int)bits.hi;
    i = i + 1;
  }
  return total + (int)(word & 0xFFFFu);
}
'''

T_NUMERIC = r'''
static int @S@_gcd(int a, int b) {
  int x = a;
  int y = b;
  while (y != 0) {
    int t = x % y;
    x = y;
    y = t;
  }
  if (x < 0) {
    return -x;
  }
  return x;
}

static long @S@_modexp(long base, long exp, long mod) {
  long result = 1;
  long b = 0;
  long e = exp;
  if (mod <= 1) {
    return 0;
  }
  b = base % mod;
  if (b < 0) {
    b = b + mod;
  }
  while (e > 0) {
    if ((e & 1) != 0) {
      result = (result * b) % mod;
    }
    b = (b * b) % mod;
    e = e >> 1;
  }
  return result;
}

static int @S@_fib(int n) {
  if (n < 2) {
    return n;
  }
  return @S@_fib(n - 1) + @S@_fib(n - 2);
}

static int @S@_sieve(u8 *flags, int limit) {
  int count = 0;
  int i = 2;
  while (i < limit) {
    flags[i] = 1;
    i = i + 1;
  }
  i = 2;
  while (i * i < limit) {
    if (flags[i] != 0) {
      int j = i * i;
      while (j < limit) {
        flags[j] = 0;
        j = j + i;
      }
    }
    i = i + 1;
  }
  i = 2;
  while (i < limit) {
    count = count + flags[i];
    i = i + 1;
  }
  return count;
}

static int @S@_kernel(void) {
  u8 flags[256];
  int total = 0;
  int i = 0;
  while (i < 24) {
    total = total + @S@_gcd(i * 13 + 7, 91);
    i = i + 1;
  }
  total = total + @S@_fib(18);
  total = total + @S@_sieve(flags, 256);
  total = total + (int)@S@_modexp(7, 129, 1000003);
  return total;
}
'''

T_SCAN = r'''
static int @S@_is_digit(char c) {
  return c >= '0' && c <= '9';
}

static int @S@_is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static int @S@_is_space(char c) {
  return c == ' ' || c == '\t' || c == '\n';
}

static int @S@_scan(const char *text, int *out, int cap) {
  int i = 0;
  int n = 0;
  while (text[i] != 0) {
    char c = text[i];
    if (@S@_is_space(c)) {
      i = i + 1;
    } else if (@S@_is_digit(c)) {
      int value = 0;
      while (@S@_is_digit(text[i])) {
        value = value * 10 + (int)(text[i] - '0');
        i = i + 1;
      }
      if (n < cap) {
        out[n] = value;
        n = n + 1;
      }
    } else if (@S@_is_alpha(c)) {
      int hash = 0;
      while (@S@_is_alpha(text[i]) || @S@_is_digit(text[i])) {
        hash = (hash * 31 + (int)(u8)text[i]) & 0x7FFFFFFF;
        i = i + 1;
      }
      if (n < cap) {
        out[n] = hash;
        n = n + 1;
      }
    } else {
      i = i + 1;
    }
  }
  return n;
}

static int @S@_kernel(void) {
  int tokens[32];
  int n = 0;
  int total = 0;
  int i = 0;
  n = @S@_scan("alpha 12 beta_3 4096 gamma 7 delta42", tokens, 32);
  total = total + n;
  while (i < n) {
    total = total + (tokens[i] & 0xFF);
    i = i + 1;
  }
  return total;
}
'''

T_GEOMETRY = r'''
static double @S@_distance2(double ax, double ay, double bx, double by) {
  double dx = ax - bx;
  double dy = ay - by;
  return dx * dx + dy * dy;
}

static double @S@_sqrt_newton(double value) {
  double guess = value;
  int i = 0;
  if (value <= 0.0) {
    return 0.0;
  }
  if (guess < 1.0) {
    guess = 1.0;
  }
  while (i < 24) {
    guess = 0.5 * (guess + value / guess);
    i = i + 1;
  }
  return guess;
}

static double @S@_area(const aurora_point *pts, int n) {
  double area = 0.0;
  int i = 0;
  if (n < 3) {
    return 0.0;
  }
  while (i < n) {
    int j = (i + 1) % n;
    area = area + (double)pts[i].x * (double)pts[j].y - (double)pts[j].x * (double)pts[i].y;
    i = i + 1;
  }
  return area * 0.5;
}

static int @S@_kernel(void) {
  aurora_point pts[12];
  double total = 0.0;
  int i = 0;
  while (i < 12) {
    pts[i] = aurora_point_make(i * 3 - 7, (i * i) % 11 - 5);
    i = i + 1;
  }
  total = total + @S@_area(pts, 12);
  i = 0;
  while (i < 12) {
    total = total + @S@_sqrt_newton(@S@_distance2(0.0, 0.0, (double)pts[i].x, (double)pts[i].y) + 1.0);
    i = i + 1;
  }
  return (int)total;
}
'''

T_OPINFO = r'''
static int @S@_tbl_add(int a, int b) { return a + b; }
static int @S@_tbl_sub(int a, int b) { return a - b; }
static int @S@_tbl_mul(int a, int b) { return a * b; }
static int @S@_tbl_max(int a, int b) { return AURORA_MAX(a, b); }
static int @S@_tbl_min(int a, int b) { return AURORA_MIN(a, b); }
static int @S@_tbl_or(int a, int b) { return a | b; }
static int @S@_tbl_and(int a, int b) { return a & b; }
static int @S@_tbl_xor(int a, int b) { return a ^ b; }

static const aurora_opinfo @S@_ops[8] = {
  { "add", @S@_tbl_add },
  { "sub", @S@_tbl_sub },
  { "mul", @S@_tbl_mul },
  { "max", @S@_tbl_max },
  { "min", @S@_tbl_min },
  { "or", @S@_tbl_or },
  { "and", @S@_tbl_and },
  { "xor", @S@_tbl_xor }
};

static int @S@_kernel(void) {
  int i = 0;
  int total = 0;
  while (i < 8) {
    aurora_binop fn = @S@_ops[i].fn;
    total = total + fn(i * 5 + 1, i + 2);
    total = total + (int)(u8)@S@_ops[i].name[0];
    i = i + 1;
  }
  return total;
}
'''

T_FSM = r'''
static int @S@_step(int state, int input) {
  switch (state) {
  case AURORA_IDLE:
    if (input > 0) {
      return AURORA_RUN;
    }
    return AURORA_IDLE;
  case AURORA_RUN:
    if (input < 0) {
      return AURORA_WAIT;
    }
    if (input == 0) {
      return AURORA_DONE;
    }
    return AURORA_RUN;
  case AURORA_WAIT:
    if (input > 16) {
      return AURORA_DONE;
    }
    if (input > 0) {
      return AURORA_RUN;
    }
    return AURORA_WAIT;
  case AURORA_DONE:
    return AURORA_IDLE;
  default:
    return AURORA_IDLE;
  }
}

static int @S@_label(int state) {
  switch (state) {
  case AURORA_IDLE:
    return 0;
  case AURORA_RUN:
    return 1;
  case AURORA_WAIT:
    return 2;
  case AURORA_DONE:
    return 3;
  default:
    return 4;
  }
}

static int @S@_kernel(void) {
  int state = AURORA_IDLE;
  int total = 0;
  int i = 0;
  while (i < 96) {
    int input = (i * 7) % 23 - 4;
    state = @S@_step(state, input);
    total = total + @S@_label(state);
    total = total + (int)(u8)aurora_titles[@S@_label(state) % 6][0];
    total = total + aurora_sign(input) + aurora_clamp(input, -3, 3);
    i = i + 1;
  }
  return total;
}
'''

T_RECORD = r'''
static void @S@_fill(aurora_entry *entry, const char *name, int score, int x, int y) {
  int i = 0;
  while (i < 15 && name[i] != 0) {
    entry->name[i] = name[i];
    i = i + 1;
  }
  entry->name[i] = 0;
  entry->score = score;
  entry->pos = aurora_point_make(x, y);
}

static int @S@_rank(const aurora_entry *entries, int n) {
  int best = -1;
  int best_index = -1;
  int i = 0;
  while (i < n) {
    if (entries[i].score > best) {
      best = entries[i].score;
      best_index = i;
    }
    i = i + 1;
  }
  return best_index;
}

static int @S@_kernel(void) {
  aurora_entry entries[16];
  aurora_pair pairs[16];
  int i = 0;
  int total = 0;
  while (i < 16) {
    entries[i].name[0] = 0;
    entries[i].score = 0;
    entries[i].pos = aurora_point_make(0, 0);
    pairs[i].first = i * 3;
    pairs[i].second = i * 5;
    i = i + 1;
  }
  @S@_fill(&entries[0], "aurora", 90, 1, 2);
  @S@_fill(&entries[1], "bench", 42, 3, 4);
  @S@_fill(&entries[2], "clang", 77, 5, 6);
  i = 3;
  while (i < 16) {
    @S@_fill(&entries[i], "unit", i * 7, i, i * 2);
    i = i + 1;
  }
  total = total + @S@_rank(entries, 16);
  i = 0;
  while (i < 16) {
    total = total + entries[i].score + entries[i].pos.x + pairs[i].first + pairs[i].second;
    i = i + 1;
  }
  return total;
}
'''

TEMPLATES = [T_BIT, T_LIST, T_BST, T_SORT, T_STRING, T_MATRIX, T_VM, T_TABLE,
             T_RING, T_FNPTR, T_BITFIELD, T_NUMERIC, T_SCAN, T_GEOMETRY,
             T_OPINFO, T_FSM, T_RECORD]

def build(target_lines):
    parts = [PROLOGUE]
    lines = PROLOGUE.count("\n")
    kernels = []
    i = 0
    while lines < target_lines or i < len(TEMPLATES):
        name = "k%02d" % i
        text = sub(TEMPLATES[i % len(TEMPLATES)], name)
        parts.append(text)
        lines += text.count("\n")
        kernels.append(name + "_kernel")
        i += 1
    main_lines = ["", "int main(void) {", "  long total = 0;"]
    main_lines.append("  aurora_pool_reset();")
    for k in kernels:
        main_lines.append("  total = total + (long)%s();" % k)
    main_lines.append("  total = total + (long)aurora_pool_bytes();")
    main_lines.append("  total = total + (long)aurora_failures;")
    main_lines.append("  aurora_checksum = total;")
    main_lines.append("  return (int)(total & 0xFFL);")
    main_lines.append("}")
    main_text = "\n".join(main_lines) + "\n"
    parts.append(main_text)
    lines += main_text.count("\n")
    return "".join(parts), kernels

out, kernels = build(2400)
out = out.lstrip("\n")
path = sys.argv[1]
with io.open(path, "w", encoding="ascii", newline="\n") as f:
    f.write(out)
print("lines=%d kernels=%d bytes=%d" % (out.count("\n"), len(kernels), len(out)))

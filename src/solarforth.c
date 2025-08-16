/*
a tiny Forth-like atop libuv

It keeps a small data stack, a simple dictionary of words,
and just enough I/O via libuv to make practical, non-blocking examples:

  timers and TCP.

Guiding ideas
- Minimal surface: only a handful of core words plus uv:* words.
- Late binding: quotations [ ... ] store tokens; names resolve when run.

Build
  cc -O2 -Wall -Wextra -std=c11 -o solarforth src/solarforth.c -luv
*/

#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

// Forward declarations for core runtime types.
typedef struct Word Word;   // A named entry in the dictionary
typedef struct Quote Quote; // A sequence of tokens to run later

typedef enum {
  VAL_INT,
  VAL_STRING,
  VAL_QUOTE,
  VAL_HANDLE,
} ValType;

typedef enum {
  HND_NONE = 0,
  HND_TIMER,
  HND_TCP,
} HandleType;

typedef struct Handle Handle;

typedef struct {
  ValType type;
  union {
    int64_t i;
    char *s;
    Quote *q;
    Handle *h;
  } as;
} Value;

// A minimal growable stack of tagged values.
typedef struct {
  Value *data; // contiguous buffer
  int top;     // index of next free slot
  int cap;     // allocated capacity
} Stack;

typedef struct Dict Dict;

// The whole VM state: stacks, dictionary, and the libuv loop.
typedef struct Context {
  Stack ds;        // data stack
  Stack rs;        // return stack (reserved)
  Dict *dict;      // dictionary of words
  uv_loop_t *loop; // libuv event loop
  bool running;    // flag to keep the REPL alive
} Context;

// A quotation is a small growable array of string tokens.
struct Quote {
  char **tokens;
  int count;
};

typedef void (*PrimFn)(Context *);

// A dictionary entry: either a C primitive or a colon definition.
struct Word {
  char *name;     // word name
  bool immediate; // reserved (not used in this tiny system)
  bool is_prim;   // true for C primitives
  PrimFn prim;    // set if is_prim
  Quote *code;    // set if colon definition
  Word *next;     // singly-linked list
};

// The dictionary is a simple singly-linked list for clarity.
struct Dict {
  Word *head;
};

// ---------------- Memory / utility helpers ----------------
// These wrappers keep the call sites clean and make failures obvious.
static void oom(void) {
  fprintf(stderr, "fatal: out of memory\n");
  exit(1);
}

static void *xmalloc(size_t n) {
  void *p = malloc(n);
  if (!p)
    oom();
  return p;
}
static void *xcalloc(size_t n, size_t sz) {
  void *p = calloc(n, sz);
  if (!p)
    oom();
  return p;
}
static char *xstrdup(const char *s) {
  size_t n = strlen(s) + 1;
  char *r = (char *)xmalloc(n);
  memcpy(r, s, n);
  return r;
}

// A tiny, growable stack used for the data stack and (reserved) return stack.
static void stack_init(Stack *s) {
  s->cap = 64;
  s->top = 0;
  s->data = (Value *)xcalloc(s->cap, sizeof(Value));
}
static void stack_free(Stack *s) {
  for (int i = 0; i < s->top; i++) {
    if (s->data[i].type == VAL_STRING && s->data[i].as.s)
      free(s->data[i].as.s);
  }
  free(s->data);
}
static void push(Stack *s, Value v) {
  if (s->top >= s->cap) {
    s->cap *= 2;
    s->data = (Value *)realloc(s->data, s->cap * sizeof(Value));
    if (!s->data)
      oom();
  }
  s->data[s->top++] = v;
}
static Value pop(Stack *s) {
  if (s->top <= 0) {
    fprintf(stderr, "stack underflow\n");
    exit(1);
  }
  return s->data[--s->top];
}
static Value peek(Stack *s) {
  if (s->top <= 0) {
    fprintf(stderr, "stack underflow\n");
    exit(1);
  }
  return s->data[s->top - 1];
}

// Construct tagged values in a compact, readable way.
static Value VInt(int64_t i) {
  Value v;
  v.type = VAL_INT;
  v.as.i = i;
  return v;
}
static Value VStrTake(char *s) {
  Value v;
  v.type = VAL_STRING;
  v.as.s = s;
  return v;
}
static Value VStr(const char *s) { return VStrTake(xstrdup(s)); }
static Value VQuote(Quote *q) {
  Value v;
  v.type = VAL_QUOTE;
  v.as.q = q;
  return v;
}

// A tiny linked-list dictionary that uses linear lookup.
static Dict *dict_new(void) {
  Dict *d = (Dict *)xcalloc(1, sizeof(Dict));
  return d;
}
static Word *dict_lookup(Dict *d, const char *name) {
  for (Word *w = d->head; w; w = w->next)
    if (strcmp(w->name, name) == 0)
      return w;
  return NULL;
}
static Word *dict_add_prim(Dict *d, const char *name, PrimFn fn,
                           bool immediate) {
  Word *w = (Word *)xcalloc(1, sizeof(Word));
  w->name = xstrdup(name);
  w->is_prim = true;
  w->prim = fn;
  w->immediate = immediate;
  w->next = d->head;
  d->head = w;
  return w;
}
static Word *dict_add_colon(Dict *d, const char *name, Quote *code) {
  Word *w = (Word *)xcalloc(1, sizeof(Word));
  w->name = xstrdup(name);
  w->is_prim = false;
  w->code = code;
  w->immediate = false;
  w->next = d->head;
  d->head = w;
  return w;
}

typedef struct {
  char **toks;
  int count;
  int cap;
  int idx;
} TokStream;

static void ts_init(TokStream *ts) {
  ts->cap = 128;
  ts->count = 0;
  ts->idx = 0;
  ts->toks = (char **)xcalloc(ts->cap, sizeof(char *));
}
static void ts_add(TokStream *ts, char *tok) {
  if (ts->count >= ts->cap) {
    ts->cap *= 2;
    ts->toks = (char **)realloc(ts->toks, ts->cap * sizeof(char *));
    if (!ts->toks)
      oom();
  }
  ts->toks[ts->count++] = tok;
}
static void ts_free(TokStream *ts) {
  for (int i = 0; i < ts->count; i++)
    free(ts->toks[i]);
  free(ts->toks);
}

static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb");
  if (!f)
    return NULL;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  char *buf = (char *)xmalloc(n + 1);
  if (fread(buf, 1, n, f) != (size_t)n) {
    fclose(f);
    free(buf);
    return NULL;
  }
  buf[n] = '\0';
  fclose(f);
  return buf;
}

// Turn source text into a flat vector of tokens.
// - Numbers are left as-is and parsed later with strtoll.
// - Strings are recognized here and encoded as a single token "#S:<text>".
// - Comments: backslash to end-of-line, or parenthesized ( ... ).
static void scan_tokens(const char *src, TokStream *ts) {
  const char *p = src;
  while (*p) {
    while (isspace((unsigned char)*p))
      p++;
    if (!*p)
      break;
    if (*p == '\\') { // line comment
      while (*p && *p != '\n')
        p++;
      continue;
    }
    if (*p == '(') { // block comment
      p++;
      while (*p && *p != ')')
        p++;
      if (*p == ')')
        p++;
      continue;
    }
    if (*p == '"') { // string literal
      p++;
      const char *start = p;
      char *out = (char *)xmalloc(1);
      size_t cap = 1, len = 0;
      while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
          char e = *p++;
          switch (e) {
          case 'n':
            c = '\n';
            break;
          case 'r':
            c = '\r';
            break;
          case 't':
            c = '\t';
            break;
          case '"':
            c = '"';
            break;
          case '\\':
            c = '\\';
            break;
          default:
            c = e;
            break;
          }
        }
        if (len + 1 >= cap) {
          cap *= 2;
          out = (char *)realloc(out, cap);
          if (!out)
            oom();
        }
        out[len++] = c;
      }
      if (*p == '"')
        p++;
      if (len + 1 >= cap) {
        cap = len + 2;
        out = (char *)realloc(out, cap);
        if (!out)
          oom();
      }
      out[len] = '\0';
      size_t outlen = len;
      char *tok = (char *)xmalloc(3 + outlen + 1);
      memcpy(tok, "#S:", 3);
      memcpy(tok + 3, out, outlen + 1);
      free(out);
      ts_add(ts, tok);
      (void)start;
      continue;
    }

    const char *start = p;
    while (*p && !isspace((unsigned char)*p)) {
      if (*p == '\\')
        break;
      p++;
    }
    size_t n = (size_t)(p - start);
    char *tok = (char *)xmalloc(n + 1);
    memcpy(tok, start, n);
    tok[n] = '\0';
    ts_add(ts, tok);
  }
}

// A quotation is a growable list of tokens. We create, append to, and free
// with 3 helpers.
static Quote *quote_new(void) {
  Quote *q = (Quote *)xcalloc(1, sizeof(Quote));
  return q;
}
static void quote_add_token(Quote *q, const char *tok) {
  q->tokens = (char **)realloc(q->tokens, (q->count + 1) * sizeof(char *));
  if (!q->tokens)
    oom();
  q->tokens[q->count++] = xstrdup(tok);
}
static void quote_free(Quote *q) {
  if (!q)
    return;
  for (int i = 0; i < q->count; i++)
    free(q->tokens[i]);
  free(q->tokens);
  free(q);
}

// A Handle bundles a libuv handle with the VM context and any associated
// quotations to run on events.
struct Handle {
  HandleType type;
  union {
    uv_handle_t base;
    uv_timer_t timer;
    uv_tcp_t tcp;
  } u;
  Quote *cb1;   // primary callback quotation
  Quote *cb2;   // optional secondary callback (unused here)
  Context *ctx; // to reach the VM from libuv callbacks
};

static Handle *handle_new(Context *ctx, HandleType t) {
  Handle *h = (Handle *)xcalloc(1, sizeof(Handle));
  h->type = t;
  h->ctx = ctx;
  return h;
}
static void handle_free(Handle *h) {
  if (!h)
    return;
  quote_free(h->cb1);
  quote_free(h->cb2);
  free(h);
}

static void exec_tokens(Context *ctx, char **tokens, int count);

// Execute a quotation by interpreting its token list.
static void exec_quote(Context *ctx, Quote *q) {
  if (!q)
    return;
  exec_tokens(ctx, q->tokens, q->count);
}

// Typed pops keep primitive implementations short and explicit.
static int64_t pop_int(Context *ctx) {
  Value v = pop(&ctx->ds);
  if (v.type != VAL_INT) {
    fprintf(stderr, "type error: expected int\n");
    exit(1);
  }
  return v.as.i;
}
static char *pop_str_take(Context *ctx) {
  Value v = pop(&ctx->ds);
  if (v.type != VAL_STRING) {
    fprintf(stderr, "type error: expected string\n");
    exit(1);
  }
  return v.as.s;
}

static Quote *pop_quote(Context *ctx) {
  Value v = pop(&ctx->ds);
  if (v.type != VAL_QUOTE) {
    fprintf(stderr, "type error: expected quote\n");
    exit(1);
  }
  return v.as.q;
}
static Handle *pop_handle(Context *ctx, HandleType want) {
  Value v = pop(&ctx->ds);
  if (v.type != VAL_HANDLE) {
    fprintf(stderr, "type error: expected handle\n");
    exit(1);
  }
  if (want != HND_NONE && v.as.h->type != want) {
    fprintf(stderr, "handle type mismatch\n");
    exit(1);
  }
  return v.as.h;
}

// A handful of words used by the examples.
static void prim_dup(Context *ctx) {
  Value v = peek(&ctx->ds);

  if (v.type == VAL_STRING)
    v.as.s = xstrdup(v.as.s);
  push(&ctx->ds, v);
}
static void prim_drop(Context *ctx) {
  Value v = pop(&ctx->ds);
  if (v.type == VAL_STRING)
    free(v.as.s);
}
static void prim_cr(Context *ctx) {
  (void)ctx;
  printf("\n");
  fflush(stdout);
}
static void prim_print(Context *ctx) {
  char *s = pop_str_take(ctx);
  fputs(s, stdout);
  free(s);
}
static void prim_bye(Context *ctx) { ctx->running = false; }

// List all defined words in the dictionary, newest first, space-separated.
static void prim_words(Context *ctx) {
  for (Word *w = ctx->dict->head; w; w = w->next) {
    fputs(w->name, stdout);
    fputc(' ', stdout);
  }
  fputc('\n', stdout);
  fflush(stdout);
}

static bool is_number(const char *t) {
  if (!*t)
    return false;
  char *end = NULL;
  errno = 0;
  long long v = strtoll(t, &end, 0);
  (void)v;
  return errno == 0 && end && *end == '\0';
}

typedef struct {
  bool compiling;
  char name[128];
  Quote *curr;
} CompileState;

// Execute a word: primitives call straight into C, colon words run quotes.
static void exec_word(Context *ctx, Word *w) {
  if (w->is_prim) {
    w->prim(ctx);
  } else {
    exec_quote(ctx, w->code);
  }
}

// Timer tick: push its handle and run the stored quotation.
static void on_timer(uv_timer_t *t) {
  Handle *h = (Handle *)t->data;
  if (!h || !h->cb1)
    return;
  Context *ctx = h->ctx;
  push(&ctx->ds, (Value){.type = VAL_HANDLE, .as.h = h});
  exec_quote(ctx, h->cb1);
}

// Run the libuv event loop and process pending I/O.
static void prim_uv_run(Context *ctx) { uv_run(ctx->loop, UV_RUN_DEFAULT); }

static void prim_uv_timer(Context *ctx) {
  Handle *h = handle_new(ctx, HND_TIMER);
  uv_timer_init(ctx->loop, &h->u.timer);
  h->u.timer.data = h;
  push(&ctx->ds, (Value){.type = VAL_HANDLE, .as.h = h});
}
static void prim_uv_timer_start(Context *ctx) {
  Quote *q = pop_quote(ctx);
  int64_t repeat = pop_int(ctx);
  int64_t timeout = pop_int(ctx);
  Handle *h = pop_handle(ctx, HND_TIMER);
  if (h->cb1)
    quote_free(h->cb1);
  h->cb1 = q;
  int rc = uv_timer_start(&h->u.timer, on_timer, (uint64_t)timeout,
                          (uint64_t)repeat);
  if (rc) {
    fprintf(stderr, "uv_timer_start: %s\n", uv_strerror(rc));
  }
}
static void prim_uv_timer_stop(Context *ctx) {
  Handle *h = pop_handle(ctx, HND_TIMER);
  int rc = uv_timer_stop(&h->u.timer);
  if (rc) {
    fprintf(stderr, "uv_timer_stop: %s\n", uv_strerror(rc));
  }
}

static void on_close_free(uv_handle_t *handle) {
  Handle *h = (Handle *)handle->data;
  handle_free(h);
}
static void prim_uv_close(Context *ctx) {
  Handle *h = pop_handle(ctx, HND_NONE);
  uv_close(&h->u.base, on_close_free);
}

// Create a TCP handle and push it.
static void prim_uv_tcp(Context *ctx) {
  Handle *h = handle_new(ctx, HND_TCP);
  uv_tcp_init(ctx->loop, &h->u.tcp);
  h->u.tcp.data = h;
  push(&ctx->ds, (Value){.type = VAL_HANDLE, .as.h = h});
}
static void prim_uv_tcp_bind(Context *ctx) {
  int64_t port = pop_int(ctx);
  char *ip = pop_str_take(ctx);
  Handle *h = pop_handle(ctx, HND_TCP);
  struct sockaddr_in addr;
  uv_ip4_addr(ip, (int)port, &addr);
  free(ip);
  int rc = uv_tcp_bind(&h->u.tcp, (const struct sockaddr *)&addr, 0);
  if (rc) {
    fprintf(stderr, "uv_tcp_bind: %s\n", uv_strerror(rc));
  }
}

// Provide a buffer to libuv’s read machinery.
static void on_alloc(uv_handle_t *handle, size_t suggested_size,
                     uv_buf_t *buf) {
  (void)handle;
  buf->base = (char *)malloc(suggested_size);
  buf->len = suggested_size;
}

static void on_write(uv_write_t *req, int status) {
  if (req->data)
    free(req->data);
  free(req);
  if (status < 0) { /* ignore */
  }
}

// When data arrives (or EOF), translate it into stack values and run the quote.
static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  Handle *h = (Handle *)stream->data;
  if (nread > 0) {
    char *s = (char *)xmalloc((size_t)nread + 1);
    memcpy(s, buf->base, (size_t)nread);
    s[nread] = '\0';
    push(&h->ctx->ds, (Value){.type = VAL_HANDLE, .as.h = h});
    push(&h->ctx->ds, VStrTake(s));
    if (h->cb1)
      exec_quote(h->ctx, h->cb1);
  } else if (nread == UV_EOF) {

    push(&h->ctx->ds, (Value){.type = VAL_HANDLE, .as.h = h});
    push(&h->ctx->ds, VStr(""));
    if (h->cb1)
      exec_quote(h->ctx, h->cb1);
    uv_read_stop(stream);
  } else if (nread < 0) { /* error */
  }
  if (buf->base)
    free(buf->base);
}

static void prim_uv_read_start(Context *ctx) {
  Quote *q = pop_quote(ctx);
  Handle *h = pop_handle(ctx, HND_TCP);
  if (h->cb1)
    quote_free(h->cb1);
  h->cb1 = q;
  int rc = uv_read_start((uv_stream_t *)&h->u.tcp, on_alloc, on_read);
  if (rc) {
    fprintf(stderr, "uv_read_start: %s\n", uv_strerror(rc));
  }
}

// Accept a new client and run the server’s quotation with the client handle.
static void on_connection(uv_stream_t *server, int status) {
  if (status < 0)
    return;
  Handle *hs = (Handle *)server->data;
  Handle *hc = handle_new(hs->ctx, HND_TCP);
  uv_tcp_init(hs->ctx->loop, &hc->u.tcp);
  hc->u.tcp.data = hc;
  if (uv_accept(server, (uv_stream_t *)&hc->u.tcp) == 0) {
    push(&hs->ctx->ds, (Value){.type = VAL_HANDLE, .as.h = hc});
    if (hs->cb1)
      exec_quote(hs->ctx, hs->cb1);
  } else {
    uv_close(&hc->u.base, on_close_free);
  }
}

static void prim_uv_listen(Context *ctx) {
  Quote *q = pop_quote(ctx);
  int64_t backlog = pop_int(ctx);
  Handle *h = pop_handle(ctx, HND_TCP);
  if (h->cb1)
    quote_free(h->cb1);
  h->cb1 = q;
  int rc = uv_listen((uv_stream_t *)&h->u.tcp, (int)backlog, on_connection);
  if (rc) {
    fprintf(stderr, "uv_listen: %s\n", uv_strerror(rc));
  }
}

typedef struct {
  uv_connect_t req;
  Handle *h;
} ConnectReq;

// Outbound connect finished: on success, run the stored quotation.
static void on_connect(uv_connect_t *req, int status) {
  ConnectReq *cr = (ConnectReq *)req;
  Handle *h = cr->h;
  if (status == 0) {
    push(&h->ctx->ds, (Value){.type = VAL_HANDLE, .as.h = h});
    if (h->cb1)
      exec_quote(h->ctx, h->cb1);
  } else { /* ignore for now */
  }
  free(cr);
}

static void prim_uv_tcp_connect(Context *ctx) {
  Quote *q = pop_quote(ctx);
  int64_t port = pop_int(ctx);
  char *ip = pop_str_take(ctx);
  Handle *h = pop_handle(ctx, HND_TCP);
  if (h->cb1)
    quote_free(h->cb1);
  h->cb1 = q;
  struct sockaddr_in dest;
  uv_ip4_addr(ip, (int)port, &dest);
  free(ip);
  ConnectReq *cr = (ConnectReq *)xcalloc(1, sizeof(ConnectReq));
  cr->h = h;
  int rc = uv_tcp_connect(&cr->req, &h->u.tcp, (const struct sockaddr *)&dest,
                          on_connect);
  if (rc) {
    fprintf(stderr, "uv_tcp_connect: %s\n", uv_strerror(rc));
    free(cr);
  }
}

static void prim_uv_write(Context *ctx) {
  char *s = pop_str_take(ctx);
  Handle *h = pop_handle(ctx, HND_TCP);
  uv_write_t *req = (uv_write_t *)xcalloc(1, sizeof(uv_write_t));
  uv_buf_t buf = uv_buf_init(s, (unsigned int)strlen(s));
  req->data = s;
  int rc = uv_write(req, (uv_stream_t *)&h->u.tcp, &buf, 1, on_write);
  if (rc) {
    fprintf(stderr, "uv_write: %s\n", uv_strerror(rc));
    free(req);
    free(s);
  }
}

static void exec_tokens(Context *ctx, char **tokens, int count) {
  CompileState cs = {0};
  for (int i = 0; i < count; i++) {
    char *t = tokens[i];
    if (cs.compiling) {
      if (strcmp(t, ";") == 0) {
        dict_add_colon(ctx->dict, cs.name, cs.curr);
        cs.compiling = false;
        cs.curr = NULL;
        continue;
      }
      if (strcmp(t, "[") == 0) {
        Quote *qq = quote_new();

        TokStream sub = {0};
        sub.toks = &tokens[i + 1];
        sub.count = count - (i + 1);
        sub.idx = 0;
        ts_init(&sub);

        int depth = 1;
        int j = i + 1;
        while (j < count) {
          if (strcmp(tokens[j], "[") == 0)
            depth++;
          else if (strcmp(tokens[j], "]") == 0) {
            depth--;
            if (depth == 0)
              break;
          }
          quote_add_token(qq, tokens[j]);
          j++;
        }
        if (depth != 0) {
          fprintf(stderr, "unclosed quote in definition\n");
          exit(1);
        }
        i = j;

        char buf[64];
        snprintf(buf, sizeof(buf), "#Q:%p", (void *)qq);
        quote_add_token(cs.curr, buf);
        continue;
      }

      quote_add_token(cs.curr, t);
      continue;
    }

    if (strcmp(t, ":") == 0) {

      if (i + 1 >= count) {
        fprintf(stderr, "expected name after :\n");
        exit(1);
      }
      i++;
      strncpy(cs.name, tokens[i], sizeof(cs.name) - 1);
      cs.name[sizeof(cs.name) - 1] = '\0';
      cs.curr = quote_new();
      cs.compiling = true;
      continue;
    }
    if (strcmp(t, "[") == 0) {
      Quote *q = quote_new();
      int depth = 1;
      int j = i + 1;
      for (; j < count; j++) {
        if (strcmp(tokens[j], "[") == 0)
          depth++;
        else if (strcmp(tokens[j], "]") == 0) {
          depth--;
          if (depth == 0)
            break;
        }
        quote_add_token(q, tokens[j]);
      }
      if (depth != 0) {
        fprintf(stderr, "unclosed quote [ ... ]\n");
        exit(1);
      }
      i = j;
      push(&ctx->ds, VQuote(q));
      continue;
    }
    if (strcmp(t, "]") == 0) {
      fprintf(stderr, "unexpected ]\n");
      exit(1);
    }
    if (is_number(t)) {
      long long v = strtoll(t, NULL, 0);
      push(&ctx->ds, VInt(v));
      continue;
    }

    Word *w = dict_lookup(ctx->dict, t);
    if (w) {
      exec_word(ctx, w);
      continue;
    }

    if (strncmp(t, "#S:", 3) == 0) {
      push(&ctx->ds, VStr(t + 3));
      continue;
    }

    if (strncmp(t, "#Q:", 3) == 0) {
      void *ptr = NULL;
      sscanf(t + 3, "%p", &ptr);
      push(&ctx->ds, VQuote((Quote *)ptr));
      continue;
    }

    fprintf(stderr, "unknown word: %s\n", t);
    exit(1);
  }
}

static void add_core_words(Context *ctx) {

  dict_add_prim(ctx->dict, "dup", prim_dup, false);
  dict_add_prim(ctx->dict, "drop", prim_drop, false);
  dict_add_prim(ctx->dict, "cr", prim_cr, false);
  dict_add_prim(ctx->dict, "print", prim_print, false);
  dict_add_prim(ctx->dict, "bye", prim_bye, false);
  dict_add_prim(ctx->dict, "words", prim_words, false);

  dict_add_prim(ctx->dict, "uv:run", prim_uv_run, false);
  dict_add_prim(ctx->dict, "uv:timer", prim_uv_timer, false);
  dict_add_prim(ctx->dict, "uv:timer-start", prim_uv_timer_start, false);
  dict_add_prim(ctx->dict, "uv:timer-stop", prim_uv_timer_stop, false);
  dict_add_prim(ctx->dict, "uv:close", prim_uv_close, false);

  dict_add_prim(ctx->dict, "uv:tcp", prim_uv_tcp, false);
  dict_add_prim(ctx->dict, "uv:tcp-bind", prim_uv_tcp_bind, false);
  dict_add_prim(ctx->dict, "uv:listen", prim_uv_listen, false);
  dict_add_prim(ctx->dict, "uv:read-start", prim_uv_read_start, false);
  dict_add_prim(ctx->dict, "uv:tcp-connect", prim_uv_tcp_connect, false);
  dict_add_prim(ctx->dict, "uv:write", prim_uv_write, false);
}

// Run a token stream through the interpreter once.
static void run_stream(Context *ctx, TokStream *ts) {
  exec_tokens(ctx, ts->toks, ts->count);
}

// A tiny prompt-loop for interactive exploration when no scripts are given.
static void repl(Context *ctx) {
  char *line = NULL;
  size_t n = 0;
  while (ctx->running) {
    printf("> ");
    fflush(stdout);
    ssize_t r = getline(&line, &n, stdin);
    if (r <= 0)
      break;
    TokStream ts = {0};
    ts_init(&ts);
    scan_tokens(line, &ts);
    run_stream(ctx, &ts);
    ts_free(&ts);
  }
  free(line);
}

int main(int argc, char **argv) {
  Context ctx = {0};
  stack_init(&ctx.ds);
  stack_init(&ctx.rs);
  ctx.dict = dict_new();
  ctx.loop = uv_default_loop();
  ctx.running = true;
  add_core_words(&ctx);

  if (argc > 1) {
    for (int i = 1; i < argc; i++) {
      char *buf = read_file(argv[i]);
      if (!buf) {
        fprintf(stderr, "cannot read %s\n", argv[i]);
        return 1;
      }
      TokStream ts = {0};
      ts_init(&ts);
      scan_tokens(buf, &ts);
      run_stream(&ctx, &ts);
      ts_free(&ts);
      free(buf);
    }
  } else {
    repl(&ctx);
  }

  stack_free(&ctx.ds);
  stack_free(&ctx.rs);
  return 0;
}

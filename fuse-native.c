#define FUSE_USE_VERSION 29

#include "semaphore.h"

#include <uv.h>
#include <node_api.h>
#include <napi-macros.h>

#include <stdio.h>
#include <stdlib.h>

#include <fuse.h>
#include <fuse_opt.h>
#include <fuse_lowlevel.h>

#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

typedef struct {
  napi_env env;
  pthread_t thread;
  pthread_attr_t attr;
  napi_ref ctx;
  napi_ref on_op;
  struct fuse *fuse;
  uv_async_t async;
} fuse_thread_t;

typedef struct {
  uint32_t ints[32];
  int32_t res;
  fuse_thread_t *fuse;
  fuse_native_semaphore_t sem;
  uv_async_t async;

  // char *reply;

} fuse_thread_locals_t;

static pthread_key_t thread_locals_key;

static void fin (napi_env env, void *fin_data, void* fin_hint) {
  printf("finaliser is run\n");
  // exit(0);
}

static void fuse_native_dispatch (uv_async_t* handle, int status) {
  fuse_thread_locals_t *l = (fuse_thread_locals_t *) handle->data;
  fuse_thread_t *ft = l->fuse;

  napi_env env = ft->env;
  napi_handle_scope scope;

  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, ft->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, ft->on_op, &callback);

  // int ptr = get_free_thread_id()
  // thread_local_map[ptr] = l

  napi_value argv[1];
  napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));

  NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 1, argv, NULL)

  napi_close_handle_scope(env, scope);
}

static fuse_thread_locals_t* get_thread_locals () {
  void *data = pthread_getspecific(thread_locals_key);

  if (data != NULL) {
    return (fuse_thread_locals_t *) data;
  }

  fuse_thread_locals_t* l = (fuse_thread_locals_t *) malloc(sizeof(fuse_thread_locals_t));

  // TODO: mutex me??
  int err = uv_async_init(uv_default_loop(), &(l->async), (uv_async_cb) fuse_native_dispatch);

  l->async.data = l;

  if (err < 0) {
    printf("uv_async failed: %i\n", err);
    return NULL;
  }

  fuse_native_semaphore_init(&(l->sem));
  pthread_setspecific(thread_locals_key, (void *) l);

  return l;
}

static void* start_fuse_thread (void *data) {
  fuse_thread_t *ft = (fuse_thread_t *) data;
  fuse_loop_mt(ft->fuse);

  // printf("her nu\n");
  // fuse_unmount(mnt, ch);
  // fuse_session_remove_chan(ch);
  // fuse_destroy(fuse);

  return NULL;
}

static int fuse_native_getattr (const char *path, struct stat *stat) {
  printf("hi\n");

  struct fuse_context *ctx = fuse_get_context();
  fuse_thread_t *ft = (fuse_thread_t *) ctx->private_data;

  fuse_thread_locals_t *l = get_thread_locals();

  l->fuse = ft;

  // b->op = OP_GETATTR;
  // b->path = (char *) path;
  // b->data = stat;

  uv_async_send(&(l->async));
  fuse_native_semaphore_wait(&(l->sem));

  printf("l->res: %i\n", l->res);
  printf("l->ints[0]: %u\n", l->ints[0]);
  printf("l->ints[1]: %u\n", l->ints[1]);
  printf("l->ints[2]: %u\n", l->ints[2]);

  return -1;
}

NAPI_METHOD(fuse_native_signal) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1)

  l->res = res;
  fuse_native_semaphore_signal(&(l->sem));

  return NULL;
}

NAPI_METHOD(fuse_native_mount) {
  NAPI_ARGV(5)

  NAPI_ARGV_UTF8(mnt, 1024, 0);
  NAPI_ARGV_UTF8(mntopts, 1024, 1);
  NAPI_ARGV_BUFFER_CAST(fuse_thread_t *, ft, 2);
  napi_create_reference(env, argv[3], 1, &(ft->ctx));
  napi_create_reference(env, argv[4], 1, &(ft->on_op));

  ft->env = env;

  struct fuse_operations ops = {
    .getattr = fuse_native_getattr
  };

  int _argc = 2;
  char *_argv[] = {
    (char *) "fuse_bindings_dummy",
    (char *) mntopts
  };

  struct fuse_args args = FUSE_ARGS_INIT(_argc, _argv);
  struct fuse_chan *ch = fuse_mount(mnt, &args);

  if (ch == NULL) {
    napi_throw_error(env, "fuse failed", "fuse failed");
    return NULL;
  }

  struct fuse *fuse = fuse_new(ch, &args, &ops, sizeof(struct fuse_operations), ft);

  ft->fuse = fuse;

  if (fuse == NULL) {
    napi_throw_error(env, "fuse failed", "fuse failed");
    return NULL;
  }

  pthread_attr_init(&(ft->attr));
  pthread_create(&(ft->thread), &(ft->attr), start_fuse_thread, ft);

  return NULL;
}

NAPI_INIT() {
  pthread_key_create(&(thread_locals_key), NULL); // TODO: add destructor

  NAPI_EXPORT_FUNCTION(fuse_native_mount)
  NAPI_EXPORT_FUNCTION(fuse_native_signal)
  NAPI_EXPORT_SIZEOF(fuse_thread_t)
}

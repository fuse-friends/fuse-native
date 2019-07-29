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

static const uint32_t op_init = 0;
static const uint32_t op_error = 1;
static const uint32_t op_access = 2;
static const uint32_t op_statfs = 3;
static const uint32_t op_fgetattr = 4;
static const uint32_t op_getattr = 5;
static const uint32_t op_flush = 6;
static const uint32_t op_fsync = 7;
static const uint32_t op_fsyncdir = 8;
static const uint32_t op_readdir = 9;
static const uint32_t op_truncate = 10;
static const uint32_t op_ftruncate = 11;
static const uint32_t op_utimens = 12;
static const uint32_t op_readlink = 13;
static const uint32_t op_chown = 14;
static const uint32_t op_chmod = 15;
static const uint32_t op_mknod = 16;
static const uint32_t op_setxattr = 17;
static const uint32_t op_getxattr = 18;
static const uint32_t op_listxattr = 19;
static const uint32_t op_removexattr = 20;
static const uint32_t op_open = 21;
static const uint32_t op_opendir = 22;
static const uint32_t op_read = 23;
static const uint32_t op_write = 24;
static const uint32_t op_release = 25;
static const uint32_t op_releasedir = 26;
static const uint32_t op_create = 27;
static const uint32_t op_unlink = 28;
static const uint32_t op_rename = 29;
static const uint32_t op_link = 30;
static const uint32_t op_symlink = 31;
static const uint32_t op_mkdir = 32;
static const uint32_t op_rmdir = 33;
static const uint32_t op_destroy = 34;

typedef struct {
  napi_env env;
  pthread_t thread;
  pthread_attr_t attr;
  napi_ref ctx;

  // Operation handlers
  napi_ref on_path_op;
  napi_ref on_stat_op;
  napi_ref on_statfs_op;
  napi_ref on_buffer_op;
  napi_ref on_readdir;
  napi_ref on_symlink;

  struct fuse *fuse;
  uv_async_t async;
} fuse_thread_t;

typedef struct {
  // Opcode
  uint32_t op;

  // Payloads
  struct stat *stat;
  struct statvfs *statvfs;
  const char *path;
  int32_t res;

  // Internal bookkeeping
  fuse_thread_t *fuse;
  fuse_native_semaphore_t sem;
  uv_async_t async;

} fuse_thread_locals_t;

static pthread_key_t thread_locals_key;

static void fin (napi_env env, void *fin_data, void* fin_hint) {
  printf("finaliser is run\n");
  // exit(0);
}

static void fuse_native_dispatch_stat(uv_async_t* handle, int status, fuse_thread_locals_t* l, fuse_thread_t* ft) {
  napi_env env = ft->env;
  napi_handle_scope scope;

  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, ft->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, ft->on_stat_op, &callback);

  napi_value argv[3];

  napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
  napi_create_uint32(env, l->op, &(argv[1]));
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));

  NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 3, argv, NULL)

  napi_close_handle_scope(env, scope);
}

static void fuse_native_dispatch_path(uv_async_t* handle, int status, fuse_thread_locals_t* l, fuse_thread_t* ft) {
  napi_env env = ft->env;
  napi_handle_scope scope;

  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, ft->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, ft->on_path_op, &callback);

  napi_value argv[3];

  napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
  napi_create_uint32(env, l->op, &(argv[1]));
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));

  NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 3, argv, NULL)

  napi_close_handle_scope(env, scope);
}

static void fuse_native_dispatch_buffer(uv_async_t* handle, int status, fuse_thread_locals_t* l, fuse_thread_t* ft) {
  napi_env env = ft->env;
  napi_handle_scope scope;

  napi_open_handle_scope(env, &scope);

  napi_value ctx;
  napi_get_reference_value(env, ft->ctx, &ctx);

  napi_value callback;
  napi_get_reference_value(env, ft->on_buffer_op, &callback);

  napi_value argv[4];

  napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
  napi_create_uint32(env, l->op, &(argv[1]));
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));

  NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 4, argv, NULL)

  napi_close_handle_scope(env, scope);
}

static void fuse_native_dispatch (uv_async_t* handle, int status) {
  fuse_thread_locals_t *l = (fuse_thread_locals_t *) handle->data;
  fuse_thread_t *ft = l->fuse;

  switch (l->op) {
    case (op_getattr):
      return fuse_native_dispatch_stat(handle, status, l, ft);
  }
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
  struct fuse_context *ctx = fuse_get_context();
  fuse_thread_t *ft = (fuse_thread_t *) ctx->private_data;

  fuse_thread_locals_t *l = get_thread_locals();

  l->fuse = ft;
  l->stat = stat;
  l->op = op_getattr;
  l->path = path;

  uv_async_send(&(l->async));
  fuse_native_semaphore_wait(&(l->sem));

  return l->res;
}

static int fuse_native_statfs (struct statvfs *statvfs) {
  struct fuse_context *ctx = fuse_get_context();
  fuse_thread_t *ft = (fuse_thread_t *) ctx->private_data;

  fuse_thread_locals_t *l = get_thread_locals();

  l->fuse = ft;
  l->statvfs = statvfs;
  l->op = op_statfs;

  uv_async_send(&(l->async));
  fuse_native_semaphore_wait(&(l->sem));

  return -1;
}

NAPI_METHOD(fuse_native_signal_path) {
  NAPI_ARGV(3)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1)

  l->res = res;
  fuse_native_semaphore_signal(&(l->sem));

  return NULL;
}

static void to_timespec (struct timespec* ts, uint32_t ms) {
  ts->tv_sec = ms / 1000;
  ts->tv_nsec = (ms % 1000) * 1000000;
}

NAPI_METHOD(fuse_native_signal_stat) {
  NAPI_ARGV(15)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);

  NAPI_ARGV_INT32(res, 1)
  NAPI_ARGV_INT32(mode, 2)
  NAPI_ARGV_INT32(uid, 3)
  NAPI_ARGV_INT32(gid, 4)
  NAPI_ARGV_INT32(size, 5)
  NAPI_ARGV_INT32(dev, 6)
  NAPI_ARGV_INT32(nlink, 7)
  NAPI_ARGV_INT32(ino, 8)
  NAPI_ARGV_INT32(rdev, 9)
  NAPI_ARGV_INT32(blksize, 10)
  NAPI_ARGV_INT32(blocks, 11)
  NAPI_ARGV_INT32(atim, 12)
  NAPI_ARGV_INT32(mtim, 13)
  NAPI_ARGV_INT32(ctim, 14)

  l->stat->st_mode = mode;
  l->stat->st_uid = uid;
  l->stat->st_gid = gid;
  l->stat->st_size = size;
  l->stat->st_dev = dev;
  l->stat->st_nlink = nlink;
  l->stat->st_ino = ino;
  l->stat->st_rdev = rdev;
  l->stat->st_blksize = blksize;
  l->stat->st_blocks = blocks;
  to_timespec(&l->stat->st_atim, atim);
  to_timespec(&l->stat->st_mtim, mtim);
  to_timespec(&l->stat->st_ctim, ctim);

  l->res = res;
  fuse_native_semaphore_signal(&(l->sem));

  return NULL;
}

NAPI_METHOD(fuse_native_signal_statfs) {
  NAPI_ARGV(15)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1)

  NAPI_ARGV_INT32(bsize, 2)
  NAPI_ARGV_INT32(frsize, 3)
  NAPI_ARGV_INT32(blocks, 4)
  NAPI_ARGV_INT32(bfree, 5)
  NAPI_ARGV_INT32(bavail, 6)
  NAPI_ARGV_INT32(files, 7)
  NAPI_ARGV_INT32(ffree, 8)
  NAPI_ARGV_INT32(favail, 9)
  NAPI_ARGV_INT32(fsid, 10)
  NAPI_ARGV_INT32(flag, 11)
  NAPI_ARGV_INT32(namemax, 12)

  l->statvfs->f_bsize = bsize;
  l->statvfs->f_frsize = frsize;
  l->statvfs->f_blocks = blocks;
  l->statvfs->f_bfree = bfree;
  l->statvfs->f_bavail = bavail;
  l->statvfs->f_files = files;
  l->statvfs->f_ffree = ffree;
  l->statvfs->f_favail = favail;
  l->statvfs->f_fsid = fsid;
  l->statvfs->f_flag = flag;
  l->statvfs->f_namemax = namemax;

  l->res = res;
  fuse_native_semaphore_signal(&(l->sem));

  return NULL;
}

NAPI_METHOD(fuse_native_signal_buffer) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1)

  l->res = res;
  fuse_native_semaphore_signal(&(l->sem));

  return NULL;
}

NAPI_METHOD(fuse_native_signal_readdir) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1)

  l->res = res;
  fuse_native_semaphore_signal(&(l->sem));

  return NULL;
}

NAPI_METHOD(fuse_native_mount) {
  NAPI_ARGV(10)

  NAPI_ARGV_UTF8(mnt, 1024, 0);
  NAPI_ARGV_UTF8(mntopts, 1024, 1);
  NAPI_ARGV_BUFFER_CAST(fuse_thread_t *, ft, 2);
  napi_create_reference(env, argv[3], 1, &(ft->ctx));

  napi_create_reference(env, argv[4], 1, &(ft->on_path_op));
  napi_create_reference(env, argv[5], 1, &(ft->on_statfs_op));
  napi_create_reference(env, argv[6], 1, &(ft->on_stat_op));
  napi_create_reference(env, argv[7], 1, &(ft->on_buffer_op));
  napi_create_reference(env, argv[8], 1, &(ft->on_readdir));
  napi_create_reference(env, argv[9], 1, &(ft->on_symlink));

  ft->env = env;

  struct fuse_operations ops = {
    .getattr = fuse_native_getattr,
    .statfs = fuse_native_statfs,
    /*
    .init = fuse_native_init,
    .error = fuse_native_error,
    .access = fuse_native_access,
    .fgetattr = fuse_native_fgetattr,
    .flush = fuse_native_flush,
    .fsync = fuse_native_fsync,
    .fsyncdir = fuse_native_fsyncdir,
    .readdir = fuse_native_readdir,
    .truncate = fuse_native_truncate,
    .ftruncate = fuse_native_ftruncate,
    .utimens = fuse_native_utimens,
    .readlink = fuse_native_readlink,
    .chown = fuse_native_chown,
    .chmod = fuse_native_chmod,
    .mknod = fuse_native_mknod,
    .setxattr = fuse_native_setxattr,
    .getxattr = fuse_native_getxattr,
    .listxattr = fuse_native_listxattr,
    .removexattr = fuse_native_removexattr,
    .open = fuse_native_open,
    .opendir = fuse_native_opendir,
    .read = fuse_native_read,
    .write = fuse_native_write,
    .release = fuse_native_release,
    .releasedir = fuse_native_releasedir,
    .create = fuse_native_create,
    .unlink = fuse_native_unlink,
    .rename = fuse_native_rename,
    .link = fuse_native_link,
    .symlink = fuse_native_symlink,
    .mkdir = fuse_native_mkdir,
    .rmdir = fuse_native_rmdir,
    .destroy = fuse_native_destroy,
    */
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
  NAPI_EXPORT_FUNCTION(fuse_native_signal_path)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_stat)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_statfs)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_buffer)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_readdir)
  NAPI_EXPORT_SIZEOF(fuse_thread_t)
  NAPI_EXPORT_UINT32(op_getattr)
  NAPI_EXPORT_UINT32(op_init)
  NAPI_EXPORT_UINT32(op_error)
  NAPI_EXPORT_UINT32(op_access)
  NAPI_EXPORT_UINT32(op_statfs)
  NAPI_EXPORT_UINT32(op_fgetattr)
  NAPI_EXPORT_UINT32(op_getattr)
  NAPI_EXPORT_UINT32(op_flush)
  NAPI_EXPORT_UINT32(op_fsync)
  NAPI_EXPORT_UINT32(op_fsyncdir)
  NAPI_EXPORT_UINT32(op_readdir)
  NAPI_EXPORT_UINT32(op_truncate)
  NAPI_EXPORT_UINT32(op_ftruncate)
  NAPI_EXPORT_UINT32(op_utimens)
  NAPI_EXPORT_UINT32(op_readlink)
  NAPI_EXPORT_UINT32(op_chown)
  NAPI_EXPORT_UINT32(op_chmod)
  NAPI_EXPORT_UINT32(op_mknod)
  NAPI_EXPORT_UINT32(op_setxattr)
  NAPI_EXPORT_UINT32(op_getxattr)
  NAPI_EXPORT_UINT32(op_listxattr)
  NAPI_EXPORT_UINT32(op_removexattr)
  NAPI_EXPORT_UINT32(op_open)
  NAPI_EXPORT_UINT32(op_opendir)
  NAPI_EXPORT_UINT32(op_read)
  NAPI_EXPORT_UINT32(op_write)
  NAPI_EXPORT_UINT32(op_release)
  NAPI_EXPORT_UINT32(op_releasedir)
  NAPI_EXPORT_UINT32(op_create)
  NAPI_EXPORT_UINT32(op_unlink)
  NAPI_EXPORT_UINT32(op_rename)
  NAPI_EXPORT_UINT32(op_link)
  NAPI_EXPORT_UINT32(op_symlink)
  NAPI_EXPORT_UINT32(op_mkdir)
  NAPI_EXPORT_UINT32(op_rmdir)
  NAPI_EXPORT_UINT32(op_destroy)
}

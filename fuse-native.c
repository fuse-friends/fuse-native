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

#define FUSE_NATIVE_CALLBACK(fn, blk)           \
  napi_env env = ft->env;                       \
  napi_handle_scope scope;                      \
  napi_open_handle_scope(env, &scope);          \
  napi_value ctx;                               \
  napi_get_reference_value(env, ft->ctx, &ctx); \
  napi_value callback;                          \
  napi_get_reference_value(env, fn, &callback); \
  blk                                           \
  napi_close_handle_scope(env, scope);

#define FUSE_NATIVE_HANDLER(blk)                            \
  struct fuse_context *ctx = fuse_get_context();            \
  fuse_thread_t *ft = (fuse_thread_t *) ctx->private_data;  \
  fuse_thread_locals_t *l = get_thread_locals();            \
  blk                                                       \
  uv_async_send(&(l->async));                               \
  fuse_native_semaphore_wait(&(l->sem));                    \
  return l->res;

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
  napi_ref on_buffer_op;
  napi_ref on_xattr_op;
  napi_ref on_statfs;
  napi_ref on_readdir;
  napi_ref on_symlink;

  struct fuse *fuse;
  struct fuse_chan *ch;
  bool mounted;
  uv_async_t async;
} fuse_thread_t;

typedef struct {
  // Opcode
  uint32_t op;

  // Payloads
  const char *path;
  struct fuse_file_info *info;
  void *buf;
  off_t offset;
  size_t len;
  mode_t mode;
  int32_t res;
  uint32_t atim[2];
  uint32_t mtim[2];

  // Extended attributes
  const char *name;
  const char *value;
  char *list;
  size_t size;
  uint32_t position;
  int flags;

  // Stat + Statfs
  struct stat *stat;
  struct statvfs *statvfs;

  // Readdir
  fuse_fill_dir_t readdir_filler;

  // Internal bookkeeping
  fuse_thread_t *fuse;
  fuse_native_semaphore_t sem;
  uv_async_t async;

} fuse_thread_locals_t;

static pthread_key_t thread_locals_key;

// Helpers
// TODO: Extract into a separate file.

static void fin (napi_env env, void *fin_data, void* fin_hint) {
  printf("finaliser is run\n");
  // exit(0);
}

static void to_timespec (struct timespec* ts, uint32_t* int_ptr) {
  long unsigned int ms = *int_ptr + (*(int_ptr + 1) * 4294967296);
  ts->tv_sec = ms / 1000;
  ts->tv_nsec = (ms % 1000) * 1000000;
}

static void from_timespec(const struct timespec* ts, uint32_t* int_ptr) {
  long unsigned int ms = (ts->tv_sec * 1000) + (ts->tv_nsec / 1000000);
  *int_ptr = ms % 4294967296;
  *(int_ptr + 1) = (ms - *int_ptr) / 4294967296;
}

static void populate_stat (uint32_t *ints, struct stat* stat) {
  stat->st_mode = *ints++;
  stat->st_uid = *ints++;
  stat->st_gid = *ints++;
  stat->st_size = *ints++;
  stat->st_dev = *ints++;
  stat->st_nlink = *ints++;
  stat->st_ino = *ints++;
  stat->st_rdev = *ints++;
  stat->st_blksize = *ints++;
  stat->st_blocks = *ints++;
  to_timespec(&stat->st_atim, ints);
  to_timespec(&stat->st_mtim, ints + 2);
  to_timespec(&stat->st_ctim, ints + 4);
}

static void populate_statvfs (uint32_t *ints, struct statvfs* statvfs) {
  statvfs->f_bsize =  *ints++;
  statvfs->f_frsize =  *ints++;
  statvfs->f_blocks =  *ints++;
  statvfs->f_bfree =  *ints++;
  statvfs->f_bavail =  *ints++;
  statvfs->f_files =  *ints++;
  statvfs->f_ffree =  *ints++;
  statvfs->f_favail =  *ints++;
  statvfs->f_fsid =  *ints++;
  statvfs->f_flag =  *ints++;
  statvfs->f_namemax =  *ints++;
}

// Dispatchers

static void fuse_native_dispatch_statfs(uv_async_t* handle, int status, fuse_thread_locals_t* l, fuse_thread_t* ft) {
  FUSE_NATIVE_CALLBACK(ft->on_statfs, {
      napi_value argv[2];

      napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
      napi_create_uint32(env, l->op, &(argv[1]));

      NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 2, argv, NULL)
  })
}

static void fuse_native_dispatch_stat(uv_async_t* handle, int status, fuse_thread_locals_t* l, fuse_thread_t* ft) {
  FUSE_NATIVE_CALLBACK(ft->on_stat_op, {
    napi_value argv[4];

    napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
    napi_create_uint32(env, l->op, &(argv[1]));
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }

    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 4, argv, NULL)
  })
}

static void fuse_native_dispatch_path(uv_async_t* handle, int status, fuse_thread_locals_t* l, fuse_thread_t* ft) {
  FUSE_NATIVE_CALLBACK(ft->on_path_op, {
    napi_value argv[7];

    napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
    napi_create_uint32(env, l->op, &(argv[1]));
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));

    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[4]));
    } else {
      napi_create_uint32(env, 0, &(argv[4]));
    }

    if (l->atim != NULL) {
      napi_create_external_arraybuffer(env, l->atim, 2 * sizeof(uint32_t), &fin, NULL, &argv[5]);
      napi_create_external_arraybuffer(env, l->mtim, 2 * sizeof(uint32_t), &fin, NULL, &argv[6]);
    } else {
      napi_create_external_arraybuffer(env, NULL, 0, &fin, NULL, &argv[5]);
      napi_create_external_arraybuffer(env, NULL, 0, &fin, NULL, &argv[6]);
    }

    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 7, argv, NULL)
  })
}

static void fuse_native_dispatch_readdir(uv_async_t* handle, int status, fuse_thread_locals_t* l, fuse_thread_t* ft) {
  FUSE_NATIVE_CALLBACK(ft->on_readdir, {
      napi_value argv[3];

      napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
      napi_create_uint32(env, l->op, &(argv[1]));
      napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));

      NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 3, argv, NULL)
  })
}

static void fuse_native_dispatch_buffer(uv_async_t* handle, int status, fuse_thread_locals_t* l, fuse_thread_t* ft) {
  FUSE_NATIVE_CALLBACK(ft->on_buffer_op, {
      napi_value argv[7];

      napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
      napi_create_uint32(env, l->op, &(argv[1]));
      napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
      napi_create_uint32(env, l->info->fh, &(argv[3]));
      napi_create_external_buffer(env, l->len, l->buf, &fin, NULL, &(argv[4]));
      napi_create_uint32(env, l->len, &(argv[5]));
      napi_create_uint32(env, l->offset, &(argv[6]));

      NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 3, argv, NULL)
  })
}

static void fuse_native_dispatch_xattr(uv_async_t* handle, int status, fuse_thread_locals_t* l, fuse_thread_t* ft) {
  FUSE_NATIVE_CALLBACK(ft->on_xattr_op, {
    napi_value argv[9];

    napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
    napi_create_uint32(env, l->op, &(argv[1]));
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
    napi_create_string_utf8(env, l->value, NAPI_AUTO_LENGTH, &(argv[4]));
    napi_create_external_buffer(env, l->size, l->list, &fin, NULL, &(argv[5]));
    napi_create_uint32(env, l->size, &(argv[6]));
    napi_create_uint32(env, l->flags, &(argv[7]));
    napi_create_uint32(env, l->position, &(argv[8]));

    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 9, argv, NULL)
  })
}

static void fuse_native_dispatch (uv_async_t* handle, int status) {
  fuse_thread_locals_t *l = (fuse_thread_locals_t *) handle->data;
  fuse_thread_t *ft = l->fuse;

  switch (l->op) {
    case (op_statfs):
      return fuse_native_dispatch_statfs(handle, status, l, ft);
    case (op_fgetattr):
    case (op_getattr):
      return fuse_native_dispatch_stat(handle, status, l, ft);
    case (op_readdir):
      return fuse_native_dispatch_readdir(handle, status, l, ft);
    case (op_open):
    case (op_create):
    case (op_access):
    case (op_utimens):
      return fuse_native_dispatch_path(handle, status, l, ft);
    case (op_release):
    case (op_releasedir):
    case (op_read):
    case (op_write):
      return fuse_native_dispatch_buffer(handle, status, l, ft);
    case (op_getxattr):
    case (op_setxattr):
    case (op_listxattr):
    case (op_removexattr):
      return fuse_native_dispatch_xattr(handle, status, l, ft);
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

// Handlers

static int fuse_native_create (const char *path, mode_t mode, struct fuse_file_info *info) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->mode = mode;
    l->info = info;
    l->op = op_create;
  })
}

static int fuse_native_access(const char *path, int mode) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->mode = mode;
    l->op = op_access;
  })
}

static int fuse_native_open(const char *path, struct fuse_file_info *info) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->info = info;
    l->op = op_open;
  })
}

static int fuse_native_release (const char *path, struct fuse_file_info *info) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->info = info;
    l->op = op_release;
  })
}

static int fuse_native_releasedir (const char *path, struct fuse_file_info *info) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->info = info;
    l->op = op_releasedir;
  })
}

static int fuse_native_write (const char *path, const char *buf, size_t len, off_t offset, struct fuse_file_info * info) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->buf = buf;
    l->len = len;
    l->offset = offset;
    l->info = info;
    l->op = op_write;
  })
}

static int fuse_native_read (const char *path, char *buf, size_t len, off_t offset, struct fuse_file_info *info) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->buf = buf;
    l->len = len;
    l->offset = offset;
    l->info = info;
    l->op = op_read;
  })
}

static int fuse_native_fgetattr (const char *path, struct stat *stat, struct fuse_file_info *info) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->stat = stat;
    l->info = info;
    l->op = op_getattr;
  })
}

static int fuse_native_statfs (struct statvfs *statvfs) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->statvfs = statvfs;
    l->op = op_statfs;
  })
}

static int fuse_native_readdir (const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *info) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->buf = buf;
    l->path = path;
    l->offset = offset;
    l->info = info;
    l->readdir_filler = filler;
    l->op = op_readdir;
  })
}

#ifdef __APPLE__
static int fuse_native_setxattr (const char *path, const char *name, const char *value, size_t size, int flags, uint32_t position) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
    l->flags = flags;
    l->position = position;
    l->op = op_setxattr;
  })
}

static int fuse_native_getxattr (const char *path, const char *name, const char *value, size_t size, uint32_t position) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
    l->position = position;
    l->op = op_getxattr;
  })
}
#else
static int fuse_native_setxattr (const char *path, const char *name, const char *value, size_t size, int flags, uint32_t position) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
    l->flags = flags;
    l->position = position;
    l->op = op_setxattr;
  })
}

static int fuse_native_getxattr (const char *path, const char *name, const char *value, size_t size, uint32_t position) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
    l->position = position;
    l->op = op_getxattr;
  })
}
#endif

static int fuse_native_listxattr (const char *path, char *list, size_t size) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->list = list;
    l->size = size;
    l->op = op_listxattr;
  })
}

static int fuse_native_removexattr (const char *path, const char *name) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    l->name = name;
    l->op = op_removexattr;
  })
}

static int fuse_native_utimens (const char *path, const struct timespec tv[2]) {
  FUSE_NATIVE_HANDLER({
    l->fuse = ft;
    l->path = path;
    from_timespec(&tv[0], l->atim);
    from_timespec(&tv[1], l->mtim);
    l->op = op_utimens;
  })
}

// Signallers

NAPI_METHOD(fuse_native_signal_path) {
  NAPI_ARGV(3)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1)
  NAPI_ARGV_INT32(fd, 2)

  l->res = res;
  if (fd != 0) {
    l->info->fh = fd;
  }
  fuse_native_semaphore_signal(&(l->sem));

  return NULL;
}

NAPI_METHOD(fuse_native_signal_stat) {
  NAPI_ARGV(4)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1)
  NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)

  populate_stat(ints, l->stat);
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

NAPI_METHOD(fuse_native_signal_xattr) {
}

NAPI_METHOD(fuse_native_signal_statfs) {
  NAPI_ARGV(3)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1)
  NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)

  populate_statvfs(ints, l->statvfs);
  l->res = res;
  fuse_native_semaphore_signal(&(l->sem));

  return NULL;
}

NAPI_METHOD(fuse_native_signal_readdir) {
  NAPI_ARGV(4)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1)

  uint32_t stats_length;
  uint32_t names_length;
  napi_get_array_length(env, argv[3], &stats_length);
  napi_get_array_length(env, argv[2], &names_length);

  napi_value raw_names = argv[2];
  napi_value raw_stats = argv[3];

  if (names_length != stats_length) {
    NAPI_FOR_EACH(raw_names, raw_name) {
      NAPI_UTF8(name, 1024, raw_name)
      int err = l->readdir_filler(l->buf, name, NULL, 0);
      if (err == 1) {
        break;
      }
    }
  } else {
    NAPI_FOR_EACH(raw_names, raw_name) {
      NAPI_UTF8(name, 1024, raw_name)
      napi_value raw_stat;
      napi_get_element(env, raw_stats, i, &raw_stat);

      NAPI_BUFFER_CAST(uint32_t*, stats_array, raw_stat);
      struct stat st;
      populate_stat(stats_array, &st);

      int err = l->readdir_filler(l->buf, name, stat, 0);
      if (err == 1) {
        break;
      }
    }
  }

  l->res = res;
  fuse_native_semaphore_signal(&(l->sem));

  return NULL;
}

NAPI_METHOD(fuse_native_mount) {
  NAPI_ARGV(11)

  NAPI_ARGV_UTF8(mnt, 1024, 0);
  NAPI_ARGV_UTF8(mntopts, 1024, 1);
  NAPI_ARGV_BUFFER_CAST(fuse_thread_t *, ft, 2);
  napi_create_reference(env, argv[3], 1, &(ft->ctx));

  napi_create_reference(env, argv[4], 1, &(ft->on_path_op));
  napi_create_reference(env, argv[5], 1, &(ft->on_stat_op));
  napi_create_reference(env, argv[6], 1, &(ft->on_buffer_op));
  napi_create_reference(env, argv[7], 1, &(ft->on_xattr_op));
  napi_create_reference(env, argv[8], 1, &(ft->on_statfs));
  napi_create_reference(env, argv[9], 1, &(ft->on_readdir));
  napi_create_reference(env, argv[10], 1, &(ft->on_symlink));

  ft->env = env;

  struct fuse_operations ops = {
    .getattr = fuse_native_fgetattr,
    .fgetattr = fuse_native_fgetattr,
    .statfs = fuse_native_statfs,
    .readdir = fuse_native_readdir,
    .open = fuse_native_open,
    .create = fuse_native_create,
    .read = fuse_native_read,
    .write = fuse_native_write,
    .release = fuse_native_release,
    .releasedir = fuse_native_releasedir,
    .access = fuse_native_access,
    .setxattr = fuse_native_setxattr,
    .getxattr = fuse_native_getxattr,
    .listxattr = fuse_native_listxattr,
    .removexattr = fuse_native_removexattr,
    .utimens = fuse_native_utimens,
    /*
    .init = fuse_native_init,
    .error = fuse_native_error,
    .flush = fuse_native_flush,
    .fsync = fuse_native_fsync,
    .fsyncdir = fuse_native_fsyncdir,
    .truncate = fuse_native_truncate,
    .ftruncate = fuse_native_ftruncate,
    .readlink = fuse_native_readlink,
    .chown = fuse_native_chown,
    .chmod = fuse_native_chmod,
    .mknod = fuse_native_mknod,
    .opendir = fuse_native_opendir,
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
  ft->ch = ch;
  ft->mounted = true;

  if (fuse == NULL) {
    napi_throw_error(env, "fuse failed", "fuse failed");
    return NULL;
  }

  pthread_attr_init(&(ft->attr));
  pthread_create(&(ft->thread), &(ft->attr), start_fuse_thread, ft);

  return NULL;
}

NAPI_METHOD(fuse_native_unmount) {
  NAPI_ARGV(2)
  NAPI_ARGV_UTF8(mnt, 1024, 0);
  NAPI_ARGV_BUFFER_CAST(fuse_thread_t *, ft, 1);

  if (ft != NULL && ft->mounted) {
    fuse_unmount(mnt, ft->ch);
    printf("joining\n");
    pthread_join(ft->thread, NULL);
    printf("joined\n");
  }
  ft->mounted = false;

  return NULL;
}

NAPI_INIT() {
  pthread_key_create(&(thread_locals_key), NULL); // TODO: add destructor

  NAPI_EXPORT_FUNCTION(fuse_native_mount)
  NAPI_EXPORT_FUNCTION(fuse_native_unmount)
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



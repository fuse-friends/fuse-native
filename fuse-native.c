#define FUSE_USE_VERSION 29

#include <uv.h>
#include <node_api.h>
#include <napi-macros.h>

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

#include <fuse.h>
#include <fuse_opt.h>
#include <fuse_common.h>
#include <fuse_lowlevel.h>

#include <unistd.h>
#include <sys/wait.h>
#include <pthread.h>

static int IS_ARRAY_BUFFER_DETACH_SUPPORTED = 0;

napi_status napi_detach_arraybuffer(napi_env env, napi_value buf);

#define FUSE_NATIVE_CALLBACK(fn, blk)\
  napi_env env = ft->env;\
  napi_handle_scope scope;\
  napi_open_handle_scope(env, &scope);\
  napi_value ctx;\
  napi_get_reference_value(env, ft->ctx, &ctx);\
  napi_value callback;\
  napi_get_reference_value(env, fn, &callback);\
  blk\
  napi_close_handle_scope(env, scope);

#define FUSE_NATIVE_HANDLER(name, blk)\
  fuse_thread_locals_t *l = get_thread_locals();\
  l->op = op_##name;\
  l->op_fn = fuse_native_dispatch_##name;\
  blk\
  uv_async_send(&(l->async));\
  uv_sem_wait(&(l->sem));\
  return l->res;

#define FUSE_METHOD(name, callbackArgs, signalArgs, signature, callBlk, callbackBlk, signalBlk)\
  static void fuse_native_dispatch_##name (uv_async_t* handle, fuse_thread_locals_t* l, fuse_thread_t* ft) {\
    uint32_t op = op_##name;\
    FUSE_NATIVE_CALLBACK(ft->handlers[op], {\
      napi_value argv[callbackArgs + 2];\
      napi_get_reference_value(env, l->self, &(argv[0]));\
      napi_create_uint32(env, l->op, &(argv[1]));\
      callbackBlk\
      NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, callbackArgs + 2, argv, NULL)\
    })\
  }\
  NAPI_METHOD(fuse_native_signal_##name) {\
    NAPI_ARGV(signalArgs + 2)\
    NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);\
    NAPI_ARGV_INT32(res, 1);\
    signalBlk\
    l->res = res;\
    uv_sem_post(&(l->sem));\
    return NULL;\
  }\
  static int fuse_native_##name signature {\
    FUSE_NATIVE_HANDLER(name, callBlk)\
  }

#define FUSE_METHOD_VOID(name, callbackArgs, signalArgs, signature, callBlk, callbackBlk)\
  FUSE_METHOD(name, callbackArgs, signalArgs, signature, callBlk, callbackBlk, {})

#define FUSE_UINT64_TO_INTS_ARGV(n, pos)\
  uint32_t low##pos = n % 4294967296;\
  uint32_t high##pos = (n - low##pos) / 4294967296;\
  napi_create_uint32(env, low##pos, &(argv[pos]));\
  napi_create_uint32(env, high##pos, &(argv[pos + 1]));


// Opcodes

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

// Data structures

typedef struct {
  napi_env env;
  pthread_t thread;
  pthread_attr_t attr;
  napi_ref ctx;
  napi_ref malloc;

  // Operation handlers
  napi_ref handlers[35];

  struct fuse *fuse;
  struct fuse_chan *ch;
  char mnt[1024];
  char mntopts[1024];
  int mounted;

  uv_async_t async;
  uv_mutex_t mut;
  uv_sem_t sem;
} fuse_thread_t;

typedef struct {
  napi_ref self;

  // Opcode
  uint32_t op;
  void *op_fn;

  // Payloads
  const char *path;
  const char *dest;
  char *linkname;
  struct fuse_file_info *info;
  const void *buf;
  off_t offset;
  size_t len;
  mode_t mode;
  dev_t dev;
  uid_t uid;
  gid_t gid;
  size_t atime;
  size_t mtime;
  int32_t res;

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
  uv_sem_t sem;
  uv_async_t async;

} fuse_thread_locals_t;

static pthread_key_t thread_locals_key;
static fuse_thread_locals_t* get_thread_locals();

// Helpers
// TODO: Extract into a separate file.

static uint64_t uint32s_to_uint64 (uint32_t **ints) {
  uint64_t low = *((*ints)++);
  uint64_t high = *((*ints)++);
  return high * 4294967296 + low;
}

static void uint32s_to_timespec (struct timespec* ts, uint32_t** ints) {
  uint64_t ms = uint32s_to_uint64(ints);
  ts->tv_sec = ms / 1000;
  ts->tv_nsec = (ms % 1000) * 1000000;
}

static uint64_t timespec_to_uint64 (const struct timespec* ts) {
  uint64_t ms = (ts->tv_sec * 1000) + (ts->tv_nsec / 1000000);
  return ms;
}

static void populate_stat (uint32_t *ints, struct stat* stat) {
  stat->st_mode = *ints++;
  stat->st_uid = *ints++;
  stat->st_gid = *ints++;
  stat->st_size = uint32s_to_uint64(&ints);
  stat->st_dev = *ints++;
  stat->st_nlink = *ints++;
  stat->st_ino = *ints++;
  stat->st_rdev = *ints++;
  stat->st_blksize = *ints++;
  stat->st_blocks = uint32s_to_uint64(&ints);
#ifdef __APPLE__
  uint32s_to_timespec(&stat->st_atimespec, &ints);
  uint32s_to_timespec(&stat->st_mtimespec, &ints);
  uint32s_to_timespec(&stat->st_ctimespec, &ints);
#else
  uint32s_to_timespec(&stat->st_atim, &ints);
  uint32s_to_timespec(&stat->st_mtim, &ints);
  uint32s_to_timespec(&stat->st_ctim, &ints);
#endif
}

static void populate_statvfs (uint32_t *ints, struct statvfs* statvfs) {
  statvfs->f_bsize = *ints++;
  statvfs->f_frsize = *ints++;
  statvfs->f_blocks = *ints++;
  statvfs->f_bfree = *ints++;
  statvfs->f_bavail = *ints++;
  statvfs->f_files = *ints++;
  statvfs->f_ffree = *ints++;
  statvfs->f_favail = *ints++;
  statvfs->f_fsid = *ints++;
  statvfs->f_flag = *ints++;
  statvfs->f_namemax = *ints++;
}

// Methods

FUSE_METHOD(statfs, 1, 1, (const char * path, struct statvfs *statvfs), {
  l->path = path;
  l->statvfs = statvfs;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
}, {
  NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)
  populate_statvfs(ints, l->statvfs);
})

FUSE_METHOD(getattr, 1, 1, (const char *path, struct stat *stat), {
  l->path = path;
  l->stat = stat;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
}, {
  NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)
  populate_stat(ints, l->stat);
})

FUSE_METHOD(fgetattr, 2, 1, (const char *path, struct stat *stat, struct fuse_file_info *info), {
  l->path = path;
  l->stat = stat;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  if (l->info != NULL) {
    napi_create_uint32(env, l->info->fh, &(argv[3]));
  } else {
    napi_create_uint32(env, 0, &(argv[3]));
  }
}, {
  NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)
  populate_stat(ints, l->stat);
})

FUSE_METHOD_VOID(access, 2, 0, (const char *path, int mode), {
  l->path = path;
  l->mode = mode;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, l->mode, &(argv[3]));
})

FUSE_METHOD(open, 2, 1, (const char *path, struct fuse_file_info *info), {
  l->path = path;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  if (l->info != NULL) {
    napi_create_uint32(env, l->info->flags, &(argv[3]));
  } else {
    napi_create_uint32(env, 0, &(argv[3]));
  }
}, {
  NAPI_ARGV_INT32(fd, 2)
  if (fd != 0) {
    l->info->fh = fd;
  }
})

FUSE_METHOD(opendir, 3, 1, (const char *path, struct fuse_file_info *info), {
  l->path = path;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  if (l->info != NULL) {
    napi_create_uint32(env, l->info->fh, &(argv[3]));
    napi_create_uint32(env, l->info->flags, &(argv[4]));
  } else {
    napi_create_uint32(env, 0, &(argv[3]));
    napi_create_uint32(env, 0, &(argv[4]));
  }
}, {
  NAPI_ARGV_INT32(fd, 2)
  if (fd != 0) {
    l->info->fh = fd;
  }
})

FUSE_METHOD(create, 2, 1, (const char *path, mode_t mode, struct fuse_file_info *info), {
  l->path = path;
  l->mode = mode;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, l->mode, &(argv[3]));
}, {
  NAPI_ARGV_INT32(fd, 2)
  if (fd != 0) {
    l->info->fh = fd;
  }
})

FUSE_METHOD_VOID(utimens, 5, 0, (const char *path, const struct timespec tv[2]), {
  l->path = path;
  l->atime = timespec_to_uint64(&tv[0]);
  l->mtime = timespec_to_uint64(&tv[1]);
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  FUSE_UINT64_TO_INTS_ARGV(l->atime, 3)
  FUSE_UINT64_TO_INTS_ARGV(l->atime, 5)
})

FUSE_METHOD_VOID(release, 2, 0, (const char *path, struct fuse_file_info *info), {
  l->path = path;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  if (l->info != NULL) {
    napi_create_uint32(env, l->info->fh, &(argv[3]));
  } else {
    napi_create_uint32(env, 0, &(argv[3]));
  }
})

FUSE_METHOD_VOID(releasedir, 2, 0, (const char *path, struct fuse_file_info *info), {
  l->path = path;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  if (l->info != NULL) {
    napi_create_uint32(env, l->info->fh, &(argv[3]));
  } else {
    napi_create_uint32(env, 0, &(argv[3]));
  }
})

FUSE_METHOD(read, 6, 2, (const char *path, char *buf, size_t len, off_t offset, struct fuse_file_info *info), {
  l->path = path;
  l->buf = buf;
  l->len = len;
  l->offset = offset;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, l->info->fh, &(argv[3]));
  napi_create_external_buffer(env, l->len, (char *) l->buf, NULL, NULL, &(argv[4]));
  napi_create_uint32(env, l->len, &(argv[5]));
  FUSE_UINT64_TO_INTS_ARGV(l->offset, 6)
}, {
  if (IS_ARRAY_BUFFER_DETACH_SUPPORTED == 1) assert(napi_detach_arraybuffer(env, argv[3]) == napi_ok);
})

FUSE_METHOD(write, 6, 2, (const char *path, const char *buf, size_t len, off_t offset, struct fuse_file_info *info), {
  l->path = path;
  l->buf = buf;
  l->len = len;
  l->offset = offset;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, l->info->fh, &(argv[3]));
  napi_create_external_buffer(env, l->len, (char *) l->buf, NULL, NULL, &(argv[4]));
  napi_create_uint32(env, l->len, &(argv[5]));
  FUSE_UINT64_TO_INTS_ARGV(l->offset, 6)
}, {
  if (IS_ARRAY_BUFFER_DETACH_SUPPORTED == 1) assert(napi_detach_arraybuffer(env, argv[3]) == napi_ok);
})

FUSE_METHOD(readdir, 1, 2, (const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *info), {
  l->buf = buf;
  l->path = path;
  l->offset = offset;
  l->info = info;
  l->readdir_filler = filler;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
}, {
  uint32_t stats_length;
  uint32_t names_length;
  napi_get_array_length(env, argv[3], &stats_length);
  napi_get_array_length(env, argv[2], &names_length);

  napi_value raw_names = argv[2];
  napi_value raw_stats = argv[3];

  if (names_length != stats_length) {
    NAPI_FOR_EACH(raw_names, raw_name) {
      NAPI_UTF8(name, 1024, raw_name)
      int err = l->readdir_filler((char *) l->buf, name, NULL, 0);
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

      // TODO: It turns out readdirplus likely won't work with FUSE 29...
      // Metadata caching between readdir/getattr will be enabled when we upgrade fuse-shared-library
      int err = l->readdir_filler((char *) l->buf, name, (struct stat *) &st, 0);
      if (err == 1) {
        break;
      }
    }
  }
})

#ifdef __APPLE__

FUSE_METHOD(setxattr, 5, 1, (const char *path, const char *name, const char *value, size_t size, int flags, uint32_t position), {
  l->path = path;
  l->name = name;
  l->value = value;
  l->size = size;
  l->flags = flags;
  l->position = position;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
  napi_create_external_buffer(env, l->size, (char *) l->value, NULL, NULL, &(argv[4]));
  napi_create_uint32(env, l->position, &(argv[5]));
  napi_create_uint32(env, l->flags, &(argv[6]));
}, {
  if (IS_ARRAY_BUFFER_DETACH_SUPPORTED == 1) assert(napi_detach_arraybuffer(env, argv[2]) == napi_ok);
})

FUSE_METHOD(getxattr, 4, 1, (const char *path, const char *name, char *value, size_t size, uint32_t position), {
  l->path = path;
  l->name = name;
  l->value = value;
  l->size = size;
  l->position = position;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
  napi_create_external_buffer(env, l->size, (char *) l->value, NULL, NULL, &(argv[4]));
  napi_create_uint32(env, l->position, &(argv[5]));
}, {
  if (IS_ARRAY_BUFFER_DETACH_SUPPORTED == 1) assert(napi_detach_arraybuffer(env, argv[2]) == napi_ok);
})

#else

FUSE_METHOD(setxattr, 5, 1, (const char *path, const char *name, const char *value, size_t size, int flags), {
  l->path = path;
  l->name = name;
  l->value = value;
  l->size = size;
  l->flags = flags;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
  napi_create_external_buffer(env, l->size, (char *) l->value, NULL, NULL, &(argv[4]));
  napi_create_uint32(env, 0, &(argv[5])); // normalize apis between mac and linux
  napi_create_uint32(env, l->flags, &(argv[6]));
}, {
  if (IS_ARRAY_BUFFER_DETACH_SUPPORTED == 1) assert(napi_detach_arraybuffer(env, argv[2]) == napi_ok);
})

FUSE_METHOD(getxattr, 4, 1, (const char *path, const char *name, char *value, size_t size), {
  l->path = path;
  l->name = name;
  l->value = value;
  l->size = size;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
  napi_create_external_buffer(env, l->size, (char *) l->value, NULL, NULL, &(argv[4]));
  napi_create_uint32(env, 0, &(argv[5]));
}, {
  if (IS_ARRAY_BUFFER_DETACH_SUPPORTED == 1) assert(napi_detach_arraybuffer(env, argv[2]) == napi_ok);
})

#endif

FUSE_METHOD(listxattr, 2, 1, (const char *path, char *list, size_t size), {
  l->path = path;
  l->list = list;
  l->size = size;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_external_buffer(env, l->size, l->list, NULL, NULL, &(argv[3]));
}, {
  if (IS_ARRAY_BUFFER_DETACH_SUPPORTED == 1) assert(napi_detach_arraybuffer(env, argv[2]) == napi_ok);
})

FUSE_METHOD_VOID(removexattr, 2, 0, (const char *path, const char *name), {
  l->path = path;
  l->name = name;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
})

FUSE_METHOD_VOID(flush, 2, 0, (const char *path, struct fuse_file_info *info), {
  l->path = path;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  if (l->info != NULL) {
    napi_create_uint32(env, l->info->fh, &(argv[3]));
  } else {
    napi_create_uint32(env, 0, &(argv[3]));
  }
})

FUSE_METHOD_VOID(fsync, 3, 0, (const char *path, int datasync, struct fuse_file_info *info), {
  l->path = path;
  l->mode = datasync;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, l->mode, &(argv[3]));
  if (l->info != NULL) {
    napi_create_uint32(env, l->info->fh, &(argv[4]));
  } else {
    napi_create_uint32(env, 0, &(argv[4]));
  }
})

FUSE_METHOD_VOID(fsyncdir, 3, 0, (const char *path, int datasync, struct fuse_file_info *info), {
  l->path = path;
  l->mode = datasync;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, l->mode, &(argv[3]));
  if (l->info != NULL) {
    napi_create_uint32(env, l->info->fh, &(argv[4]));
  } else {
    napi_create_uint32(env, 0, &(argv[4]));
  }
})


FUSE_METHOD_VOID(truncate, 3, 0, (const char *path, off_t size), {
  l->path = path;
  l->offset = size;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  FUSE_UINT64_TO_INTS_ARGV(l->offset, 3)
})

FUSE_METHOD_VOID(ftruncate, 4, 0, (const char *path, off_t size, struct fuse_file_info *info), {
  l->path = path;
  l->offset = size;
  l->info = info;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  if (l->info != NULL) {
    napi_create_uint32(env, l->info->fh, &(argv[3]));
  } else {
    napi_create_uint32(env, 0, &(argv[3]));
  }
  FUSE_UINT64_TO_INTS_ARGV(l->offset, 4)
})

FUSE_METHOD(readlink, 1, 1, (const char *path, char *linkname, size_t len), {
  l->path = path;
  l->linkname = linkname;
  l->len = len;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
}, {
  NAPI_ARGV_UTF8(linkname, l->len, 2)
  strncpy(l->linkname, linkname, l->len);
})

FUSE_METHOD_VOID(chown, 3, 0, (const char *path, uid_t uid, gid_t gid), {
  l->path = path;
  l->uid = uid;
  l->gid = gid;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, l->uid, &(argv[3]));
  napi_create_uint32(env, l->gid, &(argv[4]));
})

FUSE_METHOD_VOID(chmod, 2, 0, (const char *path, mode_t mode), {
  l->path = path;
  l->mode = mode;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, l->mode, &(argv[3]));
})

FUSE_METHOD_VOID(mknod, 3, 0, (const char *path, mode_t mode, dev_t dev), {
  l->path = path;
  l->mode = mode;
  l->dev = dev;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, l->mode, &(argv[3]));
  napi_create_uint32(env, l->dev, &(argv[4]));
})

FUSE_METHOD_VOID(unlink, 1, 0, (const char *path), {
  l->path = path;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
})

FUSE_METHOD_VOID(rename, 2, 0, (const char *path, const char *dest), {
  l->path = path;
  l->dest = dest;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_string_utf8(env, l->dest, NAPI_AUTO_LENGTH, &(argv[3]));
})

FUSE_METHOD_VOID(link, 2, 0, (const char *path, const char *dest), {
  l->path = path;
  l->dest = dest;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_string_utf8(env, l->dest, NAPI_AUTO_LENGTH, &(argv[3]));
})

FUSE_METHOD_VOID(symlink, 2, 0, (const char *path, const char *dest), {
  l->path = path;
  l->dest = dest;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_string_utf8(env, l->dest, NAPI_AUTO_LENGTH, &(argv[3]));
})

FUSE_METHOD_VOID(mkdir, 2, 0, (const char *path, mode_t mode), {
  l->path = path;
  l->mode = mode;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  napi_create_uint32(env, l->mode, &(argv[3]));
})

FUSE_METHOD_VOID(rmdir, 1, 0, (const char *path), {
  l->path = path;
}, {
  napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
})

static void fuse_native_dispatch_init (uv_async_t* handle, fuse_thread_locals_t* l, fuse_thread_t* ft) {\
  FUSE_NATIVE_CALLBACK(ft->handlers[op_init], {
    napi_value argv[2];

    napi_get_reference_value(env, l->self, &(argv[0]));
    napi_create_uint32(env, l->op, &(argv[1]));

    NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 2, argv, NULL);
  })
}

NAPI_METHOD(fuse_native_signal_init) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1);
  l->res = res;
  uv_sem_post(&(l->sem));
  return NULL;
}

static void * fuse_native_init (struct fuse_conn_info *conn) {
  fuse_thread_locals_t *l = get_thread_locals();

  l->op = op_init;
  l->op_fn = fuse_native_dispatch_init;

  uv_async_send(&(l->async));
  uv_sem_wait(&(l->sem));

  return l->fuse;
}

// Top-level dispatcher

static void fuse_native_dispatch (uv_async_t* handle) {
  fuse_thread_locals_t *l = (fuse_thread_locals_t *) handle->data;
  fuse_thread_t *ft = l->fuse;
  void (*fn)(uv_async_t *, fuse_thread_locals_t *, fuse_thread_t *) = l->op_fn;

  fn(handle, l, ft);
}

static void fuse_native_async_init (uv_async_t* handle) {
  fuse_thread_t *ft = (fuse_thread_t *) handle->data;
  fuse_thread_locals_t *l;

  FUSE_NATIVE_CALLBACK(ft->malloc, {
    napi_value argv[1];
    napi_create_uint32(ft->env, (uint32_t) sizeof(fuse_thread_locals_t), &(argv[0]));

    napi_value buf;
    NAPI_MAKE_CALLBACK(ft->env, NULL, ctx, callback, 1, argv, &buf);

    size_t l_len;

    napi_get_buffer_info(env, buf, (void **) &l, &l_len);
    napi_create_reference(env, buf, 1, &(l->self));
  })


  int err = uv_async_init(uv_default_loop(), &(l->async), (uv_async_cb) fuse_native_dispatch);
  assert(err >= 0);

  uv_unref((uv_handle_t *) &(l->async));

  uv_sem_init(&(l->sem), 0);
  l->async.data = l;
  ft->async.data = l;
  l->fuse = ft;

  uv_sem_post(&(ft->sem));
}

static fuse_thread_locals_t* get_thread_locals () {
  struct fuse_context *ctx = fuse_get_context();
  fuse_thread_t *ft = (fuse_thread_t *) ctx->private_data;

  void *data = pthread_getspecific(thread_locals_key);

  if (data != NULL) {
    return (fuse_thread_locals_t *) data;
  }

  // Need to lock the mutation of l->async.
  uv_mutex_lock(&(ft->mut));
  ft->async.data = ft;

  // Notify the main thread to uv_async_init l->async.
  uv_async_send(&(ft->async));
  uv_sem_wait(&(ft->sem));

  fuse_thread_locals_t *l = (fuse_thread_locals_t*) ft->async.data;

  pthread_setspecific(thread_locals_key, (void *) l);
  uv_mutex_unlock(&(ft->mut));

  return l;
}

static void* start_fuse_thread (void *data) {
  fuse_thread_t *ft = (fuse_thread_t *) data;
  fuse_loop_mt(ft->fuse);

  fuse_unmount(ft->mnt, ft->ch);
  fuse_session_remove_chan(ft->ch);
  fuse_destroy(ft->fuse);

  return NULL;
}

NAPI_METHOD(fuse_native_mount) {
  NAPI_ARGV(7)

  NAPI_ARGV_UTF8(mnt, 1024, 0);
  NAPI_ARGV_UTF8(mntopts, 1024, 1);
  NAPI_ARGV_BUFFER_CAST(fuse_thread_t *, ft, 2);
  napi_create_reference(env, argv[3], 1, &(ft->ctx));
  napi_create_reference(env, argv[4], 1, &(ft->malloc));
  napi_value handlers = argv[5];
  NAPI_ARGV_BUFFER_CAST(uint32_t *, implemented, 6)

  for (int i = 0; i < 35; i++) {
    ft->handlers[i] = NULL;
  }

  NAPI_FOR_EACH(handlers, handler) {
    napi_create_reference(env, handler, 1, &ft->handlers[i]);
  }

  ft->env = env;

  struct fuse_operations ops = { };
  if (implemented[op_access]) ops.access = fuse_native_access;
  if (implemented[op_truncate]) ops.truncate = fuse_native_truncate;
  if (implemented[op_ftruncate]) ops.ftruncate = fuse_native_ftruncate;
  if (implemented[op_getattr]) ops.getattr = fuse_native_getattr;
  if (implemented[op_fgetattr]) ops.fgetattr = fuse_native_fgetattr;
  if (implemented[op_flush]) ops.flush = fuse_native_flush;
  if (implemented[op_fsync]) ops.fsync = fuse_native_fsync;
  if (implemented[op_fsyncdir]) ops.fsyncdir = fuse_native_fsyncdir;
  if (implemented[op_readdir]) ops.readdir = fuse_native_readdir;
  if (implemented[op_readlink]) ops.readlink = fuse_native_readlink;
  if (implemented[op_chown]) ops.chown = fuse_native_chown;
  if (implemented[op_chmod]) ops.chmod = fuse_native_chmod;
  if (implemented[op_mknod]) ops.mknod = fuse_native_mknod;
  if (implemented[op_setxattr]) ops.setxattr = fuse_native_setxattr;
  if (implemented[op_getxattr]) ops.getxattr = fuse_native_getxattr;
  if (implemented[op_listxattr]) ops.listxattr = fuse_native_listxattr;
  if (implemented[op_removexattr]) ops.removexattr = fuse_native_removexattr;
  if (implemented[op_statfs]) ops.statfs = fuse_native_statfs;
  if (implemented[op_open]) ops.open = fuse_native_open;
  if (implemented[op_opendir]) ops.opendir = fuse_native_opendir;
  if (implemented[op_read]) ops.read = fuse_native_read;
  if (implemented[op_write]) ops.write = fuse_native_write;
  if (implemented[op_release]) ops.release = fuse_native_release;
  if (implemented[op_releasedir]) ops.releasedir = fuse_native_releasedir;
  if (implemented[op_create]) ops.create = fuse_native_create;
  if (implemented[op_utimens]) ops.utimens = fuse_native_utimens;
  if (implemented[op_unlink]) ops.unlink = fuse_native_unlink;
  if (implemented[op_rename]) ops.rename = fuse_native_rename;
  if (implemented[op_link]) ops.link = fuse_native_link;
  if (implemented[op_symlink]) ops.symlink = fuse_native_symlink;
  if (implemented[op_mkdir]) ops.mkdir = fuse_native_mkdir;
  if (implemented[op_rmdir]) ops.rmdir = fuse_native_rmdir;
  if (implemented[op_init]) ops.init = fuse_native_init;

  int _argc = (strcmp(mntopts, "-o") <= 0) ? 1 : 2;
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

  uv_mutex_init(&(ft->mut));
  uv_sem_init(&(ft->sem), 0);

  strncpy(ft->mnt, mnt, 1024);
  strncpy(ft->mntopts, mntopts, 1024);
  ft->fuse = fuse;
  ft->ch = ch;
  ft->mounted++;

  int err = uv_async_init(uv_default_loop(), &(ft->async), (uv_async_cb) fuse_native_async_init);

  if (fuse == NULL || err < 0) {
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
    // TODO: Investigate why the FUSE thread is not always killed after fusermount.
    // pthread_join(ft->thread, NULL);
  }

  // TODO: fix the async holding the loop
  uv_unref((uv_handle_t *) &(ft->async));
  ft->mounted--;

  return NULL;
}

NAPI_INIT() {
  const napi_node_version* version;
  assert(napi_get_node_version(env, &version) == napi_ok);

  if (version->major > 12 || (version->major == 12 && version->minor >= 16)) {
    IS_ARRAY_BUFFER_DETACH_SUPPORTED = 1;
  }

  pthread_key_create(&(thread_locals_key), NULL); // TODO: add destructor

  NAPI_EXPORT_SIZEOF(fuse_thread_t)

  NAPI_EXPORT_FUNCTION(fuse_native_mount)
  NAPI_EXPORT_FUNCTION(fuse_native_unmount)

  NAPI_EXPORT_FUNCTION(fuse_native_signal_getattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_init)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_access)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_statfs)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_fgetattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_getattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_flush)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_fsync)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_fsyncdir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_readdir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_truncate)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_ftruncate)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_utimens)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_readlink)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_chown)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_chmod)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_mknod)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_setxattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_getxattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_listxattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_removexattr)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_open)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_opendir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_read)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_write)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_release)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_releasedir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_create)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_unlink)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_rename)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_link)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_symlink)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_mkdir)
  NAPI_EXPORT_FUNCTION(fuse_native_signal_rmdir)

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
}

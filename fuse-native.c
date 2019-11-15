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
  blk\
  uv_async_send(&(l->async));\
  uv_sem_wait(&(l->sem));\
  return l->res;

#define FUSE_METHOD(name, callbackArgs, signalArgs, signature, callBlk, callbackBlk, signalBlk)\
  static void fuse_native_dispatch_##name (uv_async_t* handle, fuse_thread_locals_t* l, fuse_thread_t* ft) {\
    uint32_t op = op_##name;\
    FUSE_NATIVE_CALLBACK(ft->handlers[op], {\
      napi_value argv[callbackArgs + 2];\
      napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));\
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
static const uint32_t op_destroy = 34;

// Data structures

typedef struct {
  napi_env env;
  pthread_t thread;
  pthread_attr_t attr;
  napi_ref ctx;

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
  // Opcode
  uint32_t op;

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
  uint32_t atim[2];
  uint32_t mtim[2];
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
static fuse_thread_locals_t* get_thread_locals ();

// Helpers
// TODO: Extract into a separate file.

static void fin (napi_env env, void *fin_data, void* fin_hint) {
  //exit(0);
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
#ifdef __APPLE__
  to_timespec(&stat->st_atimespec, ints);
  to_timespec(&stat->st_mtimespec, ints + 2);
  to_timespec(&stat->st_ctimespec, ints + 4);
#else
  to_timespec(&stat->st_atim, ints);
  to_timespec(&stat->st_mtim, ints + 2);
  to_timespec(&stat->st_ctim, ints + 4);
#endif
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

// Methods

FUSE_METHOD(statfs, 1, 1, (const char * path, struct statvfs *statvfs), {
    l->path = path;
    l->statvfs = statvfs;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  },
  {
    NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)
    populate_statvfs(ints, l->statvfs);
  })

FUSE_METHOD(getattr, 1, 1, (const char *path, struct stat *stat), {
    l->path = path;
    l->stat = stat;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  },
  {
    NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)
    populate_stat(ints, l->stat);
  })

FUSE_METHOD(fgetattr, 2, 1, (const char *path, struct stat *stat, struct fuse_file_info *info), {
    l->path = path;
    l->stat = stat;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }
  },
  {
    NAPI_ARGV_BUFFER_CAST(uint32_t*, ints, 2)
    populate_stat(ints, l->stat);
  })

FUSE_METHOD(access, 2, 0, (const char *path, int mode), {
    l->path = path;
    l->mode = mode;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
  },
  {})

FUSE_METHOD(open, 2, 1, (const char *path, struct fuse_file_info *info), {
    l->path = path;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->flags, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }
  },
  {
    NAPI_ARGV_INT32(fd, 2)
    if (fd != 0) {
      l->info->fh = fd;
    }
  })

FUSE_METHOD(opendir, 3, 1, (const char *path, struct fuse_file_info *info), {
    l->path = path;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
      napi_create_uint32(env, l->info->flags, &(argv[4]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
      napi_create_uint32(env, 0, &(argv[4]));
    }
  },
  {
    NAPI_ARGV_INT32(fd, 2)
    if (fd != 0) {
      l->info->fh = fd;
    }
  })

FUSE_METHOD(create, 2, 1, (const char *path, mode_t mode, struct fuse_file_info *info), {
    l->path = path;
    l->mode = mode;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
  },
  {
    NAPI_ARGV_INT32(fd, 2)
    if (fd != 0) {
      l->info->fh = fd;
    }
  })

FUSE_METHOD(utimens, 3, 0, (const char *path, const struct timespec tv[2]), {
    l->path = path;
    from_timespec(&tv[0], l->atim);
    from_timespec(&tv[1], l->mtim);
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_external_arraybuffer(env, l->atim, 2 * sizeof(uint32_t), &fin, NULL, &argv[3]);
    napi_create_external_arraybuffer(env, l->mtim, 2 * sizeof(uint32_t), &fin, NULL, &argv[4]);
  },
  {})

FUSE_METHOD(release, 2, 0, (const char *path, struct fuse_file_info *info), {
    l->path = path;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }
  },
  {})

FUSE_METHOD(releasedir, 2, 0, (const char *path, struct fuse_file_info *info), {
    l->path = path;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }
  },
  {})

FUSE_METHOD(read, 5, 1, (const char *path, char *buf, size_t len, off_t offset, struct fuse_file_info *info), {
    l->path = path;
    l->buf = buf;
    l->len = len;
    l->offset = offset;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->info->fh, &(argv[3]));
    napi_create_external_buffer(env, l->len, (char *) l->buf, &fin, NULL, &(argv[4]));
    napi_create_uint32(env, l->len, &(argv[5]));
    napi_create_uint32(env, l->offset, &(argv[6]));
  },
  {
    // TODO: handle bytes processed?
  })

FUSE_METHOD(write, 5, 1, (const char *path, const char *buf, size_t len, off_t offset, struct fuse_file_info *info), {
    l->path = path;
    l->buf = buf;
    l->len = len;
    l->offset = offset;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->info->fh, &(argv[3]));
    napi_create_external_buffer(env, l->len, (char *) l->buf, &fin, NULL, &(argv[4]));
    napi_create_uint32(env, l->len, &(argv[5]));
    napi_create_uint32(env, l->offset, &(argv[6]));
  },
  {
    // TODO: handle bytes processed?
  })

FUSE_METHOD(readdir, 1, 2, (const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *info), {
    l->buf = buf;
    l->path = path;
    l->offset = offset;
    l->info = info;
    l->readdir_filler = filler;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  },
  {
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

FUSE_METHOD(setxattr, 6, 0, (const char *path, const char *name, const char *value, size_t size, int flags, uint32_t position), {
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
    l->flags = flags;
    l->position = position;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
    napi_create_string_utf8(env, l->value, NAPI_AUTO_LENGTH, &(argv[4]));
    napi_create_uint32(env, l->size, &(argv[5]));
    napi_create_uint32(env, l->flags, &(argv[6]));
    napi_create_uint32(env, l->position, &(argv[7]));
  },
  {})

FUSE_METHOD(getxattr, 5, 0, (const char *path, const char *name, char *value, size_t size, uint32_t position), {
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
    l->position = position;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
    napi_create_string_utf8(env, l->value, NAPI_AUTO_LENGTH, &(argv[4]));
    napi_create_uint32(env, l->size, &(argv[5]));
    napi_create_uint32(env, l->position, &(argv[6]));
  },
  {})

#else

FUSE_METHOD(setxattr, 5, 0, (const char *path, const char *name, const char *value, size_t size, int flags), {
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
    l->flags = flags;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
    napi_create_string_utf8(env, l->value, NAPI_AUTO_LENGTH, &(argv[4]));
    napi_create_uint32(env, l->size, &(argv[5]));
    napi_create_uint32(env, l->flags, &(argv[6]));
  },
  {})

FUSE_METHOD(getxattr, 4, 0, (const char *path, const char *name, char *value, size_t size), {
    l->path = path;
    l->name = name;
    l->value = value;
    l->size = size;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
    napi_create_string_utf8(env, l->value, NAPI_AUTO_LENGTH, &(argv[4]));
    napi_create_uint32(env, l->size, &(argv[5]));
  },
  {})

#endif

FUSE_METHOD(listxattr, 3,  0, (const char *path, char *list, size_t size), {
    l->path = path;
    l->list = list;
    l->size = size;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_external_buffer(env, l->size, l->list, &fin, NULL, &(argv[3]));
    napi_create_uint32(env, l->size, &(argv[4]));
  },
  {})

FUSE_METHOD(removexattr, 2, 0, (const char *path, const char *name), {
    l->path = path;
    l->name = name;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->name, NAPI_AUTO_LENGTH, &(argv[3]));
  },
  {})

FUSE_METHOD(flush, 2, 0, (const char *path, struct fuse_file_info *info), {
    l->path = path;
    l->info = info;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[3]));
    } else {
      napi_create_uint32(env, 0, &(argv[3]));
    }
  },
  {})

FUSE_METHOD(fsync, 3, 0, (const char *path, int datasync, struct fuse_file_info *info), {
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
  },
  {})

FUSE_METHOD(fsyncdir, 3, 0, (const char *path, int datasync, struct fuse_file_info *info), {
    l->path = path;
    l->mode = datasync;
    l->info = info;
  },
  {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[4]));
    } else {
      napi_create_uint32(env, 0, &(argv[4]));
    }
  },
  {})


FUSE_METHOD(truncate, 2, 0, (const char *path, off_t size), {
    l->path = path;
    l->len = size;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->len, &(argv[3]));
  },
  {})

FUSE_METHOD(ftruncate, 2, 0, (const char *path, off_t size, struct fuse_file_info *info), {
    l->path = path;
    l->len = size;
    l->info = info;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->len, &(argv[3]));
    if (l->info != NULL) {
      napi_create_uint32(env, l->info->fh, &(argv[4]));
    } else {
      napi_create_uint32(env, 0, &(argv[4]));
    }
  },
  {})

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

FUSE_METHOD(chown, 3, 0, (const char *path, uid_t uid, gid_t gid), {
    l->path = path;
    l->uid = uid;
    l->gid = gid;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->uid, &(argv[3]));
    napi_create_uint32(env, l->gid, &(argv[4]));
  },
  {})

FUSE_METHOD(chmod, 2, 0, (const char *path, mode_t mode), {
    l->path = path;
    l->mode = mode;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
  },
  {})

FUSE_METHOD(mknod, 3, 0, (const char *path, mode_t mode, dev_t dev), {
    l->path = path;
    l->mode = mode;
    l->dev = dev;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
    napi_create_uint32(env, l->dev, &(argv[4]));
  },
  {})

FUSE_METHOD(unlink, 1, 0, (const char *path), {
    l->path = path;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  },
  {})

FUSE_METHOD(rename, 2, 0, (const char *path, const char *dest), {
    l->path = path;
    l->dest = dest;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->dest, NAPI_AUTO_LENGTH, &(argv[3]));
  },
  {})

FUSE_METHOD(link, 2, 0, (const char *path, const char *dest), {
    l->path = path;
    l->dest = dest;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->dest, NAPI_AUTO_LENGTH, &(argv[3]));
  },
  {})

FUSE_METHOD(symlink, 2, 0, (const char *path, const char *dest), {
    l->path = path;
    l->dest = dest;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_string_utf8(env, l->dest, NAPI_AUTO_LENGTH, &(argv[3]));
  },
  {})

FUSE_METHOD(mkdir, 2, 0, (const char *path, mode_t mode), {
    l->path = path;
    l->mode = mode;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
    napi_create_uint32(env, l->mode, &(argv[3]));
  },
  {})

FUSE_METHOD(rmdir, 1, 0, (const char *path), {
    l->path = path;
  }, {
    napi_create_string_utf8(env, l->path, NAPI_AUTO_LENGTH, &(argv[2]));
  },
  {})

static void fuse_native_dispatch_init (uv_async_t* handle, fuse_thread_locals_t* l, fuse_thread_t* ft) {\
  FUSE_NATIVE_CALLBACK(ft->handlers[op_init], {
      napi_value argv[2];
      napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
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
  uv_async_send(&(l->async));
  uv_sem_wait(&(l->sem));
  return l->fuse;
}

static void fuse_native_dispatch_destroy (uv_async_t* handle, fuse_thread_locals_t* l, fuse_thread_t* ft) {\
  FUSE_NATIVE_CALLBACK(ft->handlers[op_destroy], {
      napi_value argv[2];
      napi_create_external_buffer(env, sizeof(fuse_thread_locals_t), l, &fin, NULL, &(argv[0]));
      napi_create_uint32(env, l->op, &(argv[1]));
      NAPI_MAKE_CALLBACK(env, NULL, ctx, callback, 2, argv, NULL);
    })
    }

NAPI_METHOD(fuse_native_signal_destroy) {
  NAPI_ARGV(2)
  NAPI_ARGV_BUFFER_CAST(fuse_thread_locals_t *, l, 0);
  NAPI_ARGV_INT32(res, 1);
  l->res = res;
  uv_sem_post(&(l->sem));
  return NULL;
}

static void fuse_native_destroy (void *data) {
  fuse_thread_locals_t *l = get_thread_locals();
  l->op = op_init;
  uv_async_send(&(l->async));
  uv_sem_wait(&(l->sem));
}

// Top-level dispatcher
// TODO: Generate this with a macro

static void fuse_native_dispatch (uv_async_t* handle) {
  fuse_thread_locals_t *l = (fuse_thread_locals_t *) handle->data;
  fuse_thread_t *ft = l->fuse;

  // TODO: Either use a function pointer (like ft->handlers[op]) or generate with a macro.
  switch (l->op) {
    case (op_init): return fuse_native_dispatch_init(handle, l, ft);
    case (op_statfs): return fuse_native_dispatch_statfs(handle, l, ft);
    case (op_fgetattr): return fuse_native_dispatch_fgetattr(handle, l, ft);
    case (op_getattr): return fuse_native_dispatch_getattr(handle, l, ft);
    case (op_readdir): return fuse_native_dispatch_readdir(handle, l, ft);
    case (op_open): return fuse_native_dispatch_open(handle, l, ft);
    case (op_create): return fuse_native_dispatch_create(handle, l, ft);
    case (op_access): return fuse_native_dispatch_access(handle, l, ft);
    case (op_utimens): return fuse_native_dispatch_utimens(handle, l, ft);
    case (op_release): return fuse_native_dispatch_release(handle, l, ft);
    case (op_releasedir): return fuse_native_dispatch_releasedir(handle, l, ft);
    case (op_read): return fuse_native_dispatch_read(handle, l, ft);
    case (op_write): return fuse_native_dispatch_write(handle, l, ft);
    case (op_getxattr): return fuse_native_dispatch_getxattr(handle, l, ft);
    case (op_setxattr): return fuse_native_dispatch_setxattr(handle, l, ft);
    case (op_listxattr): return fuse_native_dispatch_listxattr(handle, l, ft);
    case (op_removexattr): return fuse_native_dispatch_removexattr(handle, l, ft);
    case (op_flush): return fuse_native_dispatch_flush(handle, l, ft);
    case (op_fsync): return fuse_native_dispatch_fsync(handle, l, ft);
    case (op_fsyncdir): return fuse_native_dispatch_fsyncdir(handle, l, ft);
    case (op_truncate): return fuse_native_dispatch_truncate(handle, l, ft);
    case (op_ftruncate): return fuse_native_dispatch_ftruncate(handle, l, ft);
    case (op_readlink): return fuse_native_dispatch_readlink(handle, l, ft);
    case (op_chown): return fuse_native_dispatch_chown(handle, l, ft);
    case (op_chmod): return fuse_native_dispatch_chmod(handle, l, ft);
    case (op_mknod): return fuse_native_dispatch_mknod(handle, l, ft);
    case (op_opendir): return fuse_native_dispatch_opendir(handle, l, ft);
    case (op_unlink): return fuse_native_dispatch_unlink(handle, l, ft);
    case (op_rename): return fuse_native_dispatch_rename(handle, l, ft);
    case (op_link): return fuse_native_dispatch_link(handle, l, ft);
    case (op_symlink): return fuse_native_dispatch_symlink(handle, l, ft);
    case (op_mkdir): return fuse_native_dispatch_mkdir(handle, l, ft);
    case (op_rmdir): return fuse_native_dispatch_rmdir(handle, l, ft);
    case (op_destroy): return fuse_native_dispatch_destroy(handle, l, ft);
    default: return;
  }
}

static void fuse_native_async_init (uv_async_t* handle) {
  fuse_thread_locals_t *l = (fuse_thread_locals_t *) handle->data;
  fuse_thread_t *ft = l->fuse;

  int err = uv_async_init(uv_default_loop(), &(l->async), (uv_async_cb) fuse_native_dispatch);
  assert(err >= 0);

  uv_unref((uv_handle_t *) &(l->async));

  uv_sem_init(&(l->sem), 0);
  l->async.data = l;

  uv_sem_post(&(ft->sem));
}

static fuse_thread_locals_t* get_thread_locals () {
  struct fuse_context *ctx = fuse_get_context();
  fuse_thread_t *ft = (fuse_thread_t *) ctx->private_data;

  void *data = pthread_getspecific(thread_locals_key);

  if (data != NULL) {
    return (fuse_thread_locals_t *)data;
  }

  fuse_thread_locals_t* l = (fuse_thread_locals_t *) malloc(sizeof(fuse_thread_locals_t));
  l->fuse = ft;

  // Need to lock the mutation of l->async.
  uv_mutex_lock(&(ft->mut));
  ft->async.data = l;

  // Notify the main thread to uv_async_init l->async.
  uv_async_send(&(ft->async));
  uv_sem_wait(&(ft->sem));

  l->async.data = l;

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
  NAPI_ARGV(6)

  NAPI_ARGV_UTF8(mnt, 1024, 0);
  NAPI_ARGV_UTF8(mntopts, 1024, 1);
  NAPI_ARGV_BUFFER_CAST(fuse_thread_t *, ft, 2);
  napi_create_reference(env, argv[3], 1, &(ft->ctx));
  napi_value handlers = argv[4];
  NAPI_ARGV_BUFFER_CAST(uint32_t *, implemented, 5)

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
  if (implemented[op_destroy]) ops.destroy = fuse_native_destroy;

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

  uv_unref((uv_handle_t *) &(ft->async));
  ft->mounted--;

  return NULL;
}

NAPI_INIT() {
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
  NAPI_EXPORT_FUNCTION(fuse_native_signal_destroy)

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



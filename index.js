const os = require('os')
const fs = require('fs')
const path = require('path')

const IS_OSX = os.platform() === 'darwin'
const OSX_FOLDER_ICON = '/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericFolderIcon.icns'

const HAS_FOLDER_ICON = IS_OSX && fs.existsSync(OSX_FOLDER_ICON)

const binding = require('node-gyp-build')(__dirname)
const Opcodes = new Map([
  [ 'init', 0 ],
  [ 'error', 1 ],
  [ 'access', 2 ],
  [ 'statfs', 3 ],
  [ 'fgetattr', 4 ],
  [ 'getattr', 5 ],
  [ 'flush', 6 ],
  [ 'fsync', 7 ],
  [ 'fsyncdir', 8 ],
  [ 'readdir', 9 ],
  [ 'truncate', 10 ],
  [ 'ftruncate', 11 ],
  [ 'utimens', 12 ],
  [ 'readlink', 13 ],
  [ 'chown', 14 ],
  [ 'chmod', 15 ],
  [ 'mknod', 16 ],
  [ 'setxattr', 17 ],
  [ 'getxattr', 18 ],
  [ 'listxattr', 19 ],
  [ 'removexattr', 20 ],
  [ 'open', 21 ],
  [ 'opendir', 22 ],
  [ 'read', 23 ],
  [ 'write', 24 ],
  [ 'release', 25 ],
  [ 'releasedir', 26 ],
  [ 'create', 27 ],
  [ 'unlink', 28 ],
  [ 'rename', 29 ],
  [ 'link', 30 ],
  [ 'symlink', 31 ],
  [ 'mkdir', 32 ],
  [ 'rmdir', 33 ],
  [ 'destroy', 34 ]
])

class Fuse {
  constructor (mnt, ops, opts = {}) {
    this.opts = opts
    this.mnt = path.resolve(mnt)

    this.ops = ops
    this._thread = Buffer.alloc(binding.sizeof_fuse_thread_t)
    // Keep the process alive while fuse is mounted.
    this._timer = null

    const implemented = ['init', 'error', 'getattr']
    if (ops) {
      for (const [name, code] of Opcodes) {
        if (ops[name]) implemented.push(code)
      }
    }
    this._implemented = new Set(implemented)

    // Used to determine if the user-defined callback needs to be nextTick'd.
    this._sync = true
  }

  _withDefaultOps (cb) {
    const withDefaults = { ...this.ops }

    const callback = function (err) {
      callback = noop
      setImmediate(cb.bind(null, err))
    }

    var init = this.ops.init || call
    withDefaults.init = function (next) {
      console.log('IN INIT')
      callback()
      if (init.length > 1) init(this.mnt, next) // backwards compat for now
      else init(next)
    }

    var error = this.ops.error || call
    withDefaults.error = function (next) {
      console.log('IN ERROR')
      callback(new Error('Mount failed'))
      error(next)
    }

    if (!this.ops.getattr) { // we need this for unmount to work on osx
      withDefaults.getattr = function (path, cb) {
        if (path !== '/') return cb(Fuse.EPERM)
        cb(null, { mtime: new Date(0), atime: new Date(0), ctime: new Date(0), mode: 16877, size: 4096 })
      }
    }

    return withDefaults
  }

  _fuseOptions () {
    const options = []

    if ((/\*|(^,)fuse-bindings(,$)/.test(process.env.DEBUG)) || this.opts.debug) options.push('debug')
    if (this.opts.allowOther) options.push('allow_other')
    if (this.opts.allowRoot) options.push('allow_root')
    if (this.opts.autoUnmount) options.push('auto_unmount')
    if (this.opts.defaultPermissions) options.push('default_permissions')
    if (this.opts.blkdev) options.push('blkdev')
    if (this.opts.blksize) options.push('blksize=' + this.opts.blksize)
    if (this.opts.maxRead) options.push('max_read=' + this.opts.maxRead)
    if (this.opts.fd) options.push('fd=' + this.opts.fd)
    if (this.opts.userId) options.push('user_id=', this.opts.userId)
    if (this.opts.fsname) options.push('fsname=' + this.opts.fsname)
    if (this.opts.subtype) options.push('subtype=' + this.opts.subtype)
    if (this.opts.kernelCache) options.push('kernel_cache')
    if (this.opts.autoCache) options.push('auto_cache')
    if (this.opts.umask) options.push('umask=' + this.opts.umask)
    if (this.opts.uid) options.push('uid=' + this.opts.uid)
    if (this.opts.gid) options.push('gid=' + this.opts.gid)
    if (this.opts.entryTimeout) options.push('entry_timeout=' + this.opts.entryTimeout)
    if (this.opts.attrTimeout) options.push('attr_timeout=' + this.opts.attrTimeout)
    if (this.opts.acAttrTimeout) options.push('ac_attr_timeout=' + this.opts.acAttrTimeout)
    if (this.opts.noforget) options.push('noforget')
    if (this.opts.remember) options.push('remember=' + this.opts.remember)
    if (this.opts.modules) options.push('modules=' + this.opts.modules)

    if (this.opts.displayFolder && IS_OSX) { // only works on osx
      options.push('volname=' + path.basename(this.mnt))
      if (HAS_FOLDER_ICON) options.push('volicon=' + OSX_FOLDER_ICON)
    }

    return options.map(o => '-o' + o).join(' ')
  }

  _signal (signalFunc, args) {
    /*
    if (this._sync) process.nextTick(() => signalFunc.apply(null, args))
    else signalFunc.apply(null, args)
    */
    process.nextTick(() => signalFunc.apply(null, args))
  }

  mount (cb) {
    this.ops = this._withDefaultOps(this.ops, cb)
    const opts = this._fuseOptions()

    fs.stat(this.mnt, (err, stat) => {
      if (err) return cb(new Error('Mountpoint does not exist'))
      if (!stat.isDirectory()) return cb(new Error('Mountpoint is not a directory'))
      fs.stat(path.join(this.mnt, '..'), (_, parent) => {
        if (parent && parent.dev !== stat.dev) return cb(new Error('Mountpoint in use'))
        try {
          // TODO: asyncify
          binding.fuse_native_mount(this.mnt, opts, this._thread, this,
                                this.on_path_op, this.on_stat_op, this.on_fd_op, this.on_xattr_op,
                                this.on_statfs, this.on_readdir, this.on_symlink)
        } catch (err) {
          return cb(err)
        }
        this._timer = setInterval(() => {}, 10000)
        return cb(null)
      })
    })
  }

  unmount (cb) {
    // TODO: asyncify
    try {
      binding.fuse_native_unmount(this.mnt, this._thread)
    } catch (err) {
      clearInterval(this._timer)
      return process.nextTick(cb, err)
    }
    clearInterval(this._timer)
    return process.nextTick(cb, null)
  }

  errno (code) {
    return (code && Fuse[code.toUpperCase()]) || -1
  }

  on_symlink (handle, op, path, target) {
    const signalFunc = binding.fuse_native_signal_path.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1])
  }

  on_readdir (handle, op, path) {
    const signalFunc = binding.fuse_native_signal_readdir.bind(binding)

    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1])

    this.ops.readdir(path, (err, names, stats) => {
      if (stats) stats = stats.map(getStatArray)
      return this._signal(signalFunc, [handle, err, names, stats || []])
    })
  }

  on_statfs (handle, op) {
    const signalFunc = binding.fuse_native_signal_statfs.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1, ...getStatfsArray()])

    this.ops.statfs((err, statfs) => {
      const arr = getStatfsArray(statfs)
      return this._signal(signalFunc, [handle, err, arr])
    })
  }

  on_fd_op (handle, op, path, fd, buf, len, offset) {
    const signalFunc = binding.fuse_native_signal_buffer.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1])

    const cb = (bytesProcessed) => {
      return this._signal(signalFunc, [handle, bytesProcessed || 0])
    }

    switch (op) {
      case (binding.op_read):
        this.ops.read(path, fd, buf, len, offset, cb)
        break
      case (binding.op_write):
        this.ops.write(path, fd, buf, len, offset, cb)
        break
      case(binding.op_release):
        this.ops.release(path, fd, cb)
        break
      case(binding.op_releasedir):
        this.ops.releasedir(path, fd, cb)
        break
      default:
        return this._signal(signalFunc, [handle, 0])
    }
  }

  on_stat_op (handle, op, path, fd) {
    const signalFunc = binding.fuse_native_signal_stat.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1, getStatArray()])

    const cb = (err, stat) => {
      const arr = getStatArray(stat)
      return this._signal(signalFunc, [handle, err, arr])
    }

    switch (op) {
      case (binding.op_getattr):
        this.ops.getattr(path, cb)
        break
      case (binding.op_fgetattr):
        this.ops.fgetattr(path, fd, cb)
      default:
        return this._signal(signalFunc, [handle, -1, getStatArray()])
    }
  }

  on_path_op (handle, op, path, mode, flags, atim, mtim) {
    const signalFunc = binding.fuse_native_signal_path.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1, 0])

    const cb = (err, fd) => {
      return this._signal(signalFunc, [handle, err, fd || 0])
    }

    switch (op) {
      case (binding.op_open):
        this.ops.open(path, flags, cb)
        break
      case (binding.op_create):
        this.ops.create(path, mode, cb)
        break
      case (binding.op_access):
        this.ops.access(path, mode, cb)
        break
      case (binding.op_utimens):
        this.ops.utimens(path, getDoubleInt(atim, 0), getDoubleInt(mtim, 0), cb)
        break
      default:
        return this._signal(signalFunc, [handle, -1, 0])
    }
  }

  on_xattr_op (handle, op, path, name, value, list, size, flags, position) {
    const signalFunc = binding.fuse_native_signal_xattr.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1, 0])

    const cb = err => {
      return this._signal(signalFunc, [handle, -1])
    }

    switch (op) {
      case (binding.op_setxattr):
        this.ops.setxattr(path, name, value, size, position || 0, flags, cb)
        break
      case (binding.op_getxattr):
        this.ops.getxattr(path, name, value, size, position || 0, cb)
        break
      case (binding.op_listxattr):
        this.ops.listxattr(path, list, size, cb)
        break
      case (binding.op_removexattr):
        this.ops.removexattr(path, name, cb)
        break
      default:
        return this._signal(signalFunc, [handle, -1])
    }
  }
}

Fuse.EPERM = -1
Fuse.ENOENT = -2
Fuse.ESRCH = -3
Fuse.EINTR = -4
Fuse.EIO = -5
Fuse.ENXIO = -6
Fuse.E2BIG = -7
Fuse.ENOEXEC = -8
Fuse.EBADF = -9
Fuse.ECHILD = -10
Fuse.EAGAIN = -11
Fuse.ENOMEM = -12
Fuse.EACCES = -13
Fuse.EFAULT = -14
Fuse.ENOTBLK = -15
Fuse.EBUSY = -16
Fuse.EEXIST = -17
Fuse.EXDEV = -18
Fuse.ENODEV = -19
Fuse.ENOTDIR = -20
Fuse.EISDIR = -21
Fuse.EINVAL = -22
Fuse.ENFILE = -23
Fuse.EMFILE = -24
Fuse.ENOTTY = -25
Fuse.ETXTBSY = -26
Fuse.EFBIG = -27
Fuse.ENOSPC = -28
Fuse.ESPIPE = -29
Fuse.EROFS = -30
Fuse.EMLINK = -31
Fuse.EPIPE = -32
Fuse.EDOM = -33
Fuse.ERANGE = -34
Fuse.EDEADLK = -35
Fuse.ENAMETOOLONG = -36
Fuse.ENOLCK = -37
Fuse.ENOSYS = -38
Fuse.ENOTEMPTY = -39
Fuse.ELOOP = -40
Fuse.EWOULDBLOCK = -11
Fuse.ENOMSG = -42
Fuse.EIDRM = -43
Fuse.ECHRNG = -44
Fuse.EL2NSYNC = -45
Fuse.EL3HLT = -46
Fuse.EL3RST = -47
Fuse.ELNRNG = -48
Fuse.EUNATCH = -49
Fuse.ENOCSI = -50
Fuse.EL2HLT = -51
Fuse.EBADE = -52
Fuse.EBADR = -53
Fuse.EXFULL = -54
Fuse.ENOANO = -55
Fuse.EBADRQC = -56
Fuse.EBADSLT = -57
Fuse.EDEADLOCK = -35
Fuse.EBFONT = -59
Fuse.ENOSTR = -60
Fuse.ENODATA = -61
Fuse.ETIME = -62
Fuse.ENOSR = -63
Fuse.ENONET = -64
Fuse.ENOPKG = -65
Fuse.EREMOTE = -66
Fuse.ENOLINK = -67
Fuse.EADV = -68
Fuse.ESRMNT = -69
Fuse.ECOMM = -70
Fuse.EPROTO = -71
Fuse.EMULTIHOP = -72
Fuse.EDOTDOT = -73
Fuse.EBADMSG = -74
Fuse.EOVERFLOW = -75
Fuse.ENOTUNIQ = -76
Fuse.EBADFD = -77
Fuse.EREMCHG = -78
Fuse.ELIBACC = -79
Fuse.ELIBBAD = -80
Fuse.ELIBSCN = -81
Fuse.ELIBMAX = -82
Fuse.ELIBEXEC = -83
Fuse.EILSEQ = -84
Fuse.ERESTART = -85
Fuse.ESTRPIPE = -86
Fuse.EUSERS = -87
Fuse.ENOTSOCK = -88
Fuse.EDESTADDRREQ = -89
Fuse.EMSGSIZE = -90
Fuse.EPROTOTYPE = -91
Fuse.ENOPROTOOPT = -92
Fuse.EPROTONOSUPPORT = -93
Fuse.ESOCKTNOSUPPORT = -94
Fuse.EOPNOTSUPP = -95
Fuse.EPFNOSUPPORT = -96
Fuse.EAFNOSUPPORT = -97
Fuse.EADDRINUSE = -98
Fuse.EADDRNOTAVAIL = -99
Fuse.ENETDOWN = -100
Fuse.ENETUNREACH = -101
Fuse.ENETRESET = -102
Fuse.ECONNABORTED = -103
Fuse.ECONNRESET = -104
Fuse.ENOBUFS = -105
Fuse.EISCONN = -106
Fuse.ENOTCONN = -107
Fuse.ESHUTDOWN = -108
Fuse.ETOOMANYREFS = -109
Fuse.ETIMEDOUT = -110
Fuse.ECONNREFUSED = -111
Fuse.EHOSTDOWN = -112
Fuse.EHOSTUNREACH = -113
Fuse.EALREADY = -114
Fuse.EINPROGRESS = -115
Fuse.ESTALE = -116
Fuse.EUCLEAN = -117
Fuse.ENOTNAM = -118
Fuse.ENAVAIL = -119
Fuse.EISNAM = -120
Fuse.EREMOTEIO = -121
Fuse.EDQUOT = -122
Fuse.ENOMEDIUM = -123
Fuse.EMEDIUMTYPE = -124

module.exports = Fuse

function getStatfsArray (statfs) {
  const ints = new Uint32Array(11)

  ints[0] = (statfs && statfs.bsize) || 0
  ints[1] = (statfs && statfs.frsize) || 0
  ints[2] = (statfs && statfs.blocks) || 0
  ints[3] = (statfs && statfs.bfree) || 0
  ints[4] = (statfs && statfs.bavail) || 0
  ints[5] = (statfs && statfs.files) || 0
  ints[6] = (statfs && statfs.ffree) || 0
  ints[7] = (statfs && statfs.favail) || 0
  ints[8] = (statfs && statfs.fsid) || 0
  ints[9] = (statfs && statfs.flag) || 0
  ints[10] = (statfs && statfs.namemax) || 0

  return ints
}

function setDoubleInt (arr, idx, num) {
  arr[idx] = num % 4294967296
  arr[idx + 1] = (num - arr[idx]) / 4294967296
}

function getDoubleInt(arr, idx) {
  arr = new Uint32Array(arr)
  var num = arr[idx + 1] * 4294967296
  num += arr[idx]
  return num
}

function getStatArray (stat) {
  const ints = new Uint32Array(16)

  ints[0] = (stat && stat.mode) || 0
  ints[1] = (stat && stat.uid) || 0
  ints[2] = (stat && stat.gid)  || 0
  ints[3] = (stat && stat.size)  || 0
  ints[4] = (stat && stat.dev)  || 0
  ints[5] = (stat && stat.nlink)  || 1
  ints[6] = (stat && stat.ino)  || 0
  ints[7] = (stat && stat.rdev)  || 0
  ints[8] = (stat && stat.blksize)  || 0
  ints[9] = (stat && stat.blocks)  || 0
  setDoubleInt(ints, 10, (stat && stat.atim) || Date.now())
  setDoubleInt(ints, 12, (stat && stat.atim) || Date.now())
  setDoubleInt(ints, 14, (stat && stat.atim) || Date.now())

  return ints
}

function noop () {}
function call (cb) { cb() }

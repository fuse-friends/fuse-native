const os = require('os')
const fs = require('fs')
const path = require('path')
const { exec } = require('child_process')

const Nanoresource = require('nanoresource')
const { beforeMount, beforeUnmount, configure, unconfigure, isConfigured } = require('fuse-shared-library')

const binding = require('node-gyp-build')(__dirname)

const IS_OSX = os.platform() === 'darwin'
const OSX_FOLDER_ICON = '/System/Library/CoreServices/CoreTypes.bundle/Contents/Resources/GenericFolderIcon.icns'
const HAS_FOLDER_ICON = IS_OSX && fs.existsSync(OSX_FOLDER_ICON)
const DEFAULT_TIMEOUT = 15 * 1000
const TIMEOUT_ERRNO = IS_OSX ? -60 : -110
const ENOTCONN = IS_OSX ? -57 : -107

const OpcodesAndDefaults = new Map([
  ['init', {
    op: binding.op_init
  }],
  ['error', {
    op: binding.op_error
  }],
  ['access', {
    op: binding.op_access,
    defaults: [0]
  }],
  ['statfs', {
    op: binding.op_statfs,
    defaults: [getStatfsArray()]
  }],
  ['fgetattr', {
    op: binding.op_fgetattr,
    defaults: [getStatArray()]
  }],
  ['getattr', {
    op: binding.op_getattr,
    defaults: [getStatArray()]
  }],
  ['flush', {
    op: binding.op_flush
  }],
  ['fsync', {
    op: binding.op_fsync
  }],
  ['fsyncdir', {
    op: binding.op_fsyncdir
  }],
  ['readdir', {
    op: binding.op_readdir,
    defaults: [[], []]
  }],
  ['truncate', {
    op: binding.op_truncate
  }],
  ['ftruncate', {
    op: binding.op_ftruncate
  }],
  ['utimens', {
    op: binding.op_utimens
  }],
  ['readlink', {
    op: binding.op_readlink,
    defaults: ['']
  }],
  ['chown', {
    op: binding.op_chown
  }],
  ['chmod', {
    op: binding.op_chmod
  }],
  ['mknod', {
    op: binding.op_mknod
  }],
  ['setxattr', {
    op: binding.op_setxattr
  }],
  ['getxattr', {
    op: binding.op_getxattr
  }],
  ['listxattr', {
    op: binding.op_listxattr
  }],
  ['removexattr', {
    op: binding.op_removexattr
  }],
  ['open', {
    op: binding.op_open,
    defaults: [0]
  }],
  ['opendir', {
    op: binding.op_opendir,
    defaults: [0]
  }],
  ['read', {
    op: binding.op_read,
    defaults: [0]
  }],
  ['write', {
    op: binding.op_write,
    defaults: [0]
  }],
  ['release', {
    op: binding.op_release
  }],
  ['releasedir', {
    op: binding.op_releasedir
  }],
  ['create', {
    op: binding.op_create,
    defaults: [0]
  }],
  ['unlink', {
    op: binding.op_unlink
  }],
  ['rename', {
    op: binding.op_rename
  }],
  ['link', {
    op: binding.op_link
  }],
  ['symlink', {
    op: binding.op_symlink
  }],
  ['mkdir', {
    op: binding.op_mkdir
  }],
  ['rmdir', {
    op: binding.op_rmdir
  }]
])

class Fuse extends Nanoresource {
  constructor (mnt, ops, opts = {}) {
    super()

    this.opts = opts
    this.mnt = path.resolve(mnt)
    this.ops = ops
    this.timeout = opts.timeout === false ? 0 : (opts.timeout || DEFAULT_TIMEOUT)

    this._force = !!opts.force
    this._mkdir = !!opts.mkdir
    this._thread = null
    this._handlers = this._makeHandlerArray()
    this._threads = new Set()

    const implemented = [binding.op_init, binding.op_error, binding.op_getattr]
    if (ops) {
      for (const [name, { op }] of OpcodesAndDefaults) {
        if (ops[name]) implemented.push(op)
      }
    }
    this._implemented = new Set(implemented)

    // Used to determine if the user-defined callback needs to be nextTick'd.
    this._sync = true
  }

  _getImplementedArray () {
    const implemented = new Uint32Array(35)
    for (const impl of this._implemented) {
      implemented[impl] = 1
    }
    return implemented
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
      options.push('volname=' + path.basename(this.opts.name || this.mnt))
      if (HAS_FOLDER_ICON) options.push('volicon=' + OSX_FOLDER_ICON)
    }

    return options.length ? '-o' + options.join(',') : ''
  }

  _malloc (size) {
    const buf = Buffer.alloc(size)
    this._threads.add(buf)
    return buf
  }

  _makeHandlerArray () {
    const self = this
    const handlers = new Array(OpcodesAndDefaults.size)

    for (const [name, { op, defaults }] of OpcodesAndDefaults) {
      const nativeSignal = binding[`fuse_native_signal_${name}`]
      if (!nativeSignal) continue

      handlers[op] = makeHandler(name, op, defaults, nativeSignal)
    }

    return handlers

    function makeHandler (name, op, defaults, nativeSignal) {
      let to = self.timeout
      if (typeof to === 'object' && to) {
        const defaultTimeout = to.default || DEFAULT_TIMEOUT
        to = to[name]
        if (!to && to !== false) to = defaultTimeout
      }

      return function (nativeHandler, opCode, ...args) {
        const sig = signal.bind(null, nativeHandler)
        const input = [...args]
        const boundSignal = to ? autoTimeout(sig, input) : sig
        const funcName = `_op_${name}`
        if (!self[funcName] || !self._implemented.has(op)) return boundSignal(-1, ...defaults)
        return self[funcName].apply(self, [boundSignal, ...args])
      }

      function signal (nativeHandler, err, ...args) {
        var arr = [nativeHandler, err, ...args]

        if (defaults) {
          while (arr.length > 2 && arr[arr.length - 1] === undefined) arr.pop()
          if (arr.length === 2) arr = arr.concat(defaults)
        }

        return process.nextTick(nativeSignal, ...arr)
      }

      function autoTimeout (cb, input) {
        let called = false
        const timeout = setTimeout(timeoutWrap, to, TIMEOUT_ERRNO)
        return timeoutWrap

        function timeoutWrap (err, ...args) {
          if (called) return
          called = true

          clearTimeout(timeout)

          if (err === TIMEOUT_ERRNO) {
            switch (name) {
              case 'write':
              case 'read':
                return cb(TIMEOUT_ERRNO, 0, input[2].buffer)
              case 'setxattr':
                return cb(TIMEOUT_ERRNO, input[2].buffer)
              case 'getxattr':
                return cb(TIMEOUT_ERRNO, input[2].buffer)
              case 'listxattr':
                return cb(TIMEOUT_ERRNO, input[1].buffer)
            }
          }

          cb(err, ...args)
        }
      }
    }
  }

  // Static methods

  static unmount (mnt, cb) {
    mnt = JSON.stringify(mnt)
    const cmd = IS_OSX ? `diskutil unmount force ${mnt}` : `fusermount -uz ${mnt}`
    exec(cmd, err => {
      if (err) return cb(err)
      return cb(null)
    })
  }

  // Debugging methods

  // Lifecycle methods

  _open (cb) {
    const self = this

    if (this._force) {
      return fs.stat(path.join(this.mnt, 'test'), (err, st) => {
        if (err && (err.errno === ENOTCONN || err.errno === Fuse.ENXIO)) return Fuse.unmount(this.mnt, open)
        return open()
      })
    }
    return open()

    function open () {
      // If there was an unmount error, continue attempting to mount (this is the best we can do)
      self._thread = Buffer.alloc(binding.sizeof_fuse_thread_t)
      self._openCallback = cb

      const opts = self._fuseOptions()
      const implemented = self._getImplementedArray()

      return fs.stat(self.mnt, (err, stat) => {
        if (err && err.errno !== -2) return cb(err)
        if (err) {
          if (!self._mkdir) return cb(new Error('Mountpoint does not exist'))
          return fs.mkdir(self.mnt, { recursive: true }, err => {
            if (err) return cb(err)
            fs.stat(self.mnt, (err, stat) => {
              if (err) return cb(err)
              return onexists(stat)
            })
          })
        }
        if (!stat.isDirectory()) return cb(new Error('Mountpoint is not a directory'))
        return onexists(stat)
      })

      function onexists (stat) {
        fs.stat(path.join(self.mnt, '..'), (_, parent) => {
          if (parent && parent.dev !== stat.dev) return cb(new Error('Mountpoint in use'))
          try {
            // TODO: asyncify
            binding.fuse_native_mount(self.mnt, opts, self._thread, self, self._malloc, self._handlers, implemented)
          } catch (err) {
            return cb(err)
          }
        })
      }
    }
  }

  _close (cb) {
    const self = this

    Fuse.unmount(this.mnt, err => {
      if (err) {
        err.unmountFailure = true
        return cb(err)
      }
      nativeUnmount()
    })

    function nativeUnmount () {
      try {
        binding.fuse_native_unmount(self.mnt, self._thread)
      } catch (err) {
        return cb(err)
      }
      return cb(null)
    }
  }

  // Handlers

  _op_init (signal) {
    if (this._openCallback) {
      process.nextTick(this._openCallback, null)
      this._openCallback = null
    }
    if (!this.ops.init) {
      signal(0)
      return
    }
    this.ops.init(err => {
      return signal(err)
    })
  }

  _op_error (signal) {
    if (!this.ops.error) {
      signal(0)
      return
    }
    this.ops.error(err => {
      return signal(err)
    })
  }

  _op_statfs (signal, path) {
    this.ops.statfs(path, (err, statfs) => {
      if (err) return signal(err)
      const arr = getStatfsArray(statfs)
      return signal(0, arr)
    })
  }

  _op_getattr (signal, path) {
    if (!this.ops.getattr) {
      if (path !== '/') {
        signal(Fuse.EPERM)
      } else {
        signal(0, getStatArray({ mtime: new Date(0), atime: new Date(0), ctime: new Date(0), mode: 16877, size: 4096 }))
      }
      return
    }

    this.ops.getattr(path, (err, stat) => {
      if (err) return signal(err, getStatArray())
      return signal(0, getStatArray(stat))
    })
  }

  _op_fgetattr (signal, path, fd) {
    if (!this.ops.fgetattr) {
      if (path !== '/') {
        signal(Fuse.EPERM)
      } else {
        signal(0, getStatArray({ mtime: new Date(0), atime: new Date(0), ctime: new Date(0), mode: 16877, size: 4096 }))
      }
      return
    }
    this.ops.getattr(path, (err, stat) => {
      if (err) return signal(err)
      return signal(0, getStatArray(stat))
    })
  }

  _op_access (signal, path, mode) {
    this.ops.access(path, mode, err => {
      return signal(err)
    })
  }

  _op_open (signal, path, flags) {
    this.ops.open(path, flags, (err, fd) => {
      return signal(err, fd)
    })
  }

  _op_opendir (signal, path, flags) {
    this.ops.opendir(path, flags, (err, fd) => {
      return signal(err, fd)
    })
  }

  _op_create (signal, path, mode) {
    this.ops.create(path, mode, (err, fd) => {
      return signal(err, fd)
    })
  }

  _op_utimens (signal, path, atimeLow, atimeHigh, mtimeLow, mtimeHigh) {
    const atime = getDoubleArg(atimeLow, atimeHigh)
    const mtime = getDoubleArg(mtimeLow, mtimeHigh)
    this.ops.utimens(path, atime, mtime, err => {
      return signal(err)
    })
  }

  _op_release (signal, path, fd) {
    this.ops.release(path, fd, err => {
      return signal(err)
    })
  }

  _op_releasedir (signal, path, fd) {
    this.ops.releasedir(path, fd, err => {
      return signal(err)
    })
  }

  _op_read (signal, path, fd, buf, len, offsetLow, offsetHigh) {
    this.ops.read(path, fd, buf, len, getDoubleArg(offsetLow, offsetHigh), (err, bytesRead) => {
      return signal(err, bytesRead || 0, buf.buffer)
    })
  }

  _op_write (signal, path, fd, buf, len, offsetLow, offsetHigh) {
    this.ops.write(path, fd, buf, len, getDoubleArg(offsetLow, offsetHigh), (err, bytesWritten) => {
      return signal(err, bytesWritten || 0, buf.buffer)
    })
  }

  _op_readdir (signal, path) {
    this.ops.readdir(path, (err, names, stats) => {
      if (err) return signal(err)
      if (stats) stats = stats.map(getStatArray)
      return signal(0, names, stats || [])
    })
  }

  _op_setxattr (signal, path, name, value, position, flags) {
    this.ops.setxattr(path, name, value, position, flags, err => {
      return signal(err, value.buffer)
    })
  }

  _op_getxattr (signal, path, name, valueBuf, position) {
    this.ops.getxattr(path, name, position, (err, value) => {
      if (!err) {
        if (!value) return signal(IS_OSX ? -93 : -61, valueBuf.buffer)
        value.copy(valueBuf)
        return signal(value.length, valueBuf.buffer)
      }
      return signal(err, valueBuf.buffer)
    })
  }

  _op_listxattr (signal, path, listBuf) {
    this.ops.listxattr(path, (err, list) => {
      if (list && !err) {
        if (!listBuf.length) {
          let size = 0
          for (const name of list) size += Buffer.byteLength(name) + 1
          size += 128 // fuse yells if we do not signal room for some mac stuff also
          return signal(size, listBuf.buffer)
        }

        let ptr = 0
        for (const name of list) {
          listBuf.write(name, ptr)
          ptr += Buffer.byteLength(name)
          listBuf[ptr++] = 0
        }

        return signal(ptr, listBuf.buffer)
      }
      return signal(err, listBuf.buffer)
    })
  }

  _op_removexattr (signal, path, name) {
    this.ops.removexattr(path, name, err => {
      return signal(err)
    })
  }

  _op_flush (signal, path, fd) {
    this.ops.flush(path, fd, err => {
      return signal(err)
    })
  }

  _op_fsync (signal, path, datasync, fd) {
    this.ops.fsync(path, datasync, fd, err => {
      return signal(err)
    })
  }

  _op_fsyncdir (signal, path, datasync, fd) {
    this.ops.fsyncdir(path, datasync, fd, err => {
      return signal(err)
    })
  }

  _op_truncate (signal, path, sizeLow, sizeHigh) {
    const size = getDoubleArg(sizeLow, sizeHigh)
    this.ops.truncate(path, size, err => {
      return signal(err)
    })
  }

  _op_ftruncate (signal, path, fd, sizeLow, sizeHigh) {
    const size = getDoubleArg(sizeLow, sizeHigh)
    this.ops.ftruncate(path, fd, size, err => {
      return signal(err)
    })
  }

  _op_readlink (signal, path) {
    this.ops.readlink(path, (err, linkname) => {
      return signal(err, linkname)
    })
  }

  _op_chown (signal, path, uid, gid) {
    this.ops.chown(path, uid, gid, err => {
      return signal(err)
    })
  }

  _op_chmod (signal, path, mode) {
    this.ops.chmod(path, mode, err => {
      return signal(err)
    })
  }

  _op_mknod (signal, path, mode, dev) {
    this.ops.mknod(path, mode, dev, err => {
      return signal(err)
    })
  }

  _op_unlink (signal, path) {
    this.ops.unlink(path, err => {
      return signal(err)
    })
  }

  _op_rename (signal, src, dest) {
    this.ops.rename(src, dest, err => {
      return signal(err)
    })
  }

  _op_link (signal, src, dest) {
    this.ops.link(src, dest, err => {
      return signal(err)
    })
  }

  _op_symlink (signal, src, dest) {
    this.ops.symlink(src, dest, err => {
      return signal(err)
    })
  }

  _op_mkdir (signal, path, mode) {
    this.ops.mkdir(path, mode, err => {
      return signal(err)
    })
  }

  _op_rmdir (signal, path) {
    this.ops.rmdir(path, err => {
      return signal(err)
    })
  }

  // Public API

  mount (cb) {
    return this.open(cb)
  }

  unmount (cb) {
    return this.close(cb)
  }

  errno (code) {
    return (code && Fuse[code.toUpperCase()]) || -1
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

// Forward configuration functions through the exported class.
Fuse.beforeMount = beforeMount
Fuse.beforeUnmount = beforeUnmount
Fuse.configure = configure
Fuse.unconfigure = unconfigure
Fuse.isConfigured = isConfigured

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

function getDoubleArg (a, b) {
  return a + b * 4294967296
}

function toDateMS (st) {
  if (typeof st === 'number') return st
  if (!st) return Date.now()
  return st.getTime()
}

function getStatArray (stat) {
  const ints = new Uint32Array(18)

  ints[0] = (stat && stat.mode) || 0
  ints[1] = (stat && stat.uid) || 0
  ints[2] = (stat && stat.gid) || 0
  setDoubleInt(ints, 3, (stat && stat.size) || 0)
  ints[5] = (stat && stat.dev) || 0
  ints[6] = (stat && stat.nlink) || 1
  ints[7] = (stat && stat.ino) || 0
  ints[8] = (stat && stat.rdev) || 0
  ints[9] = (stat && stat.blksize) || 0
  setDoubleInt(ints, 10, (stat && stat.blocks) || 0)
  setDoubleInt(ints, 12, toDateMS(stat && stat.atime))
  setDoubleInt(ints, 14, toDateMS(stat && stat.mtime))
  setDoubleInt(ints, 16, toDateMS(stat && stat.ctime))

  return ints
}

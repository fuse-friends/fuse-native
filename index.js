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
    this.ops = ops
    this.mnt = mnt

    this._thread = Buffer.alloc(binding.sizeof_fuse_thread_t)

    const implemented = []
    if (ops) {
      for (const [name, code] of Opcodes) {
        if (ops[name]) implemented.push(code)
      }
    }
    this._implemented = new Set(implemented)

    // Used to determine if the user-defined callback needs to be nextTick'd.
    this._sync = true
  }

  _signal (signalFunc, args) {
    /*
    if (this._sync) process.nextTick(() => signalFunc.apply(null, args))
    else signalFunc.apply(null, args)
    */
    process.nextTick(() => signalFunc.apply(null, args))
  }

  mount () {
    binding.fuse_native_mount(this.mnt, '-odebug', this._thread, this,
                              this.on_path_op, this.on_statfs_op, this.on_stat_op,
                              this.on_buffer_op, this.on_readdir, this.on_symlink)
  }

  unmount () {
    binding.fuse_native_unmount(this.mnt)
  }

  on_symlink (handle, op, path, target) {
    const signalFunc = binding.fuse_native_signal_path.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1])
  }

  on_readdir (handle, op, path) {
    const signalFunc = binding.fuse_native_signal_readdir.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1])
  }

  on_buffer_op (handle, op, path, buf) {
    const signalFunc = binding.fuse_native_signal_buffer.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1])
  }

  on_statfs_op (handle, op, path) {
    const signalFunc = binding.fuse_native_signal_statfs.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1])

    this.ops.statfs((err, statfs) => {
      const arr = getStatfsArray(statfs)
      return this._signal(signalFunc, [handle, err, ...arr])
    })
  }

  on_stat_op (handle, op, path) {
    const signalFunc = binding.fuse_native_signal_stat.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1, ...getStatArray()])

    switch (op) {
      case (binding.op_getattr):
        this.ops.getattr(path, (err, stat) => {
          const arrs = getStatArray(stat)
          return this._signal(signalFunc, [handle, err, ...arrs])
        })
        break

      default:
      return this._signal(signalFunc, [handle, -1, ...getStatArray()])
    }
  }

  on_path_op (handle, op, path) {
    const signalFunc = binding.fuse_native_signal_path.bind(binding)
    if (!this._implemented.has(op)) return this._signal(signalFunc, [handle, -1])

  }
}

function getStatfsArray (statfs) {
  const ints = Array(11)

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

function getStatArray (stat) {
  const ints = Array(13)

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
  ints[10] = (stat && stat.atim) || Date.now()
  ints[11] = (stat && stat.mtim) || Date.now()
  ints[12] = (stat && stat.ctim) || Date.now()

  return ints
}

const f = new Fuse('mnt', {
  getattr: (path, cb) => {
    return cb(0, {
      mtime: new Date(),
      atime: new Date(),
      ctime: new Date(),
      nlink: 1,
      size: 100,
      mode: 16877,
      uid: process.getuid ? process.getuid() : 0,
      gid: process.getgid ? process.getgid() : 0
    })
  }
})
f.mount()

setInterval(() => {
  if (global.gc) gc()
}, 1000)

// setTimeout(function () {
//   foo = null
//   console.log('now!')
// }, 5000)

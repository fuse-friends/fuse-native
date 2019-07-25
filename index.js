const binding = require('node-gyp-build')(__dirname)

console.log(binding)

class Fuse {
  constructor () {
    this._thread = Buffer.alloc(binding.sizeof_fuse_thread_t)
  }

  mount (mnt) {
    binding.fuse_native_mount(mnt, '-odebug', this._thread, this, this.onop)
  }

  onop (handle) {
    const ints = new Uint32Array(handle.buffer, handle.byteOffset, 32)

    ints[0] = 1
    ints[1] = 42
    ints[2] = 10

    process.nextTick(function () {
      binding.fuse_native_signal(handle, -1)
    })
  }
}

const f = new Fuse()

f.mount('mnt')

setInterval(() => {
  console.log(!!(f))
  if (global.gc) gc()
}, 1000)

// setTimeout(function () {
//   foo = null
//   console.log('now!')
// }, 5000)

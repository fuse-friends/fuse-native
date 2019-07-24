const binding = require('node-gyp-build')(__dirname)

console.log(binding)

class Fuse {
  constructor () {
    this.fuseThread = Buffer.alloc(binding.sizeof_fuse_thread_t)
  }

  mount (mnt) {
    binding.fuse_native_mount('mnt', '-odebug', data, this, on_op)
  }
}

const ctx = {}
const data = Buffer.alloc(binding.sizeof_fuse_thread_t)
let buffers = []
let foo

binding.fuse_native_mount('mnt', '-odebug', data, ctx, on_op)

let to = 1

setTimeout(function () {
  to = 5000
}, 5000)

setInterval(() => {
  console.log(!!(data && buffers && ctx && on_op))
  gc()
}, 1000)

function on_op (buf) {
  // foo = buf
  console.log('on_op is called', buf)
  // buffers.push(buf)

  setTimeout(function () {
    console.log('in timeout')
    binding.fuse_native_signal(buf)
  }, to)
}

// setTimeout(function () {
//   foo = null
//   console.log('now!')
// }, 5000)

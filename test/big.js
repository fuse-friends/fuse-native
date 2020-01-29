const tape = require('tape')
const fs = require('fs')
const path = require('path')
const concat = require('concat-stream')

const Fuse = require('../')
const createMountpoint = require('./fixtures/mnt')
const stat = require('./fixtures/stat')
const { unmount } = require('./helpers')

const mnt = createMountpoint()

tape('read and write big file', function (t) {
  let size = 0
  const reads = [0, 4 * 1024 * 1024 * 1024, 6 * 1024 * 1024 * 1024]
  const writes = [0, 4 * 1024 * 1024 * 1024, 6 * 1024 * 1024 * 1024]

  var ops = {
    force: true,
    readdir (path, cb) {
      if (path === '/') return process.nextTick(cb, null, ['test'])
      return process.nextTick(cb, Fuse.ENOENT)
    },
    getattr (path, cb) {
      if (path === '/') return process.nextTick(cb, null, stat({ mode: 'dir', size: 4096 }))
      if (path === '/test') return process.nextTick(cb, null, stat({ mode: 'file', size, mtime: new Date() }))
      return process.nextTick(cb, Fuse.ENOENT)
    },
    open (path, flags, cb) {
      return process.nextTick(cb, 0, 42)
    },
    release (path, fd, cb) {
      t.same(fd, 42, 'fd was passed to release')
      return process.nextTick(cb, 0)
    },
    read (path, fd, buf, len, pos, cb) {
      t.same(pos, reads.shift(), 'read is expected')
      buf.fill(0)
      if (pos + len > size) return cb(Math.max(size - pos, 0))
      cb(len)
    },
    ftruncate (path, fd, len, cb) {
      size = len
      cb(0)
    },
    truncate (path, len, cb) {
      size = len
      cb(0)
    },
    write (path, fd, buf, len, pos, cb) {
      if (!writes.length) return cb(-1)
      t.same(pos, writes.shift(), 'write is expected')
      size = Math.max(pos + len, size)
      cb(len)
    }
  }

  const fuse = new Fuse(mnt, ops, { debug: !true, autoCache: true })
  let fd = 0

  run(
    (_, cb) => fuse.mount(cb),
    open('w+'),
    (_, cb) => fs.fstat(fd, cb),
    checkSize(0),
    (_, cb) => fs.ftruncate(fd, 4 * 1024 * 1024 * 1024 + 1, cb),
    (_, cb) => fs.fstat(fd, cb),
    checkSize(4 * 1024 * 1024 * 1024 + 1),
    (_, cb) => fs.truncate(path.join(mnt, 'test'), 6 * 1024 * 1024 * 1024 + 2, cb),
    (_, cb) => fs.fstat(fd, cb),
    checkSize(6 * 1024 * 1024 * 1024 + 2),
    (_, cb) => fs.write(fd, Buffer.alloc(4096), 0, 4096, 0, cb),
    (_, cb) => fs.write(fd, Buffer.alloc(4096), 0, 4096, 4 * 1024 * 1024 * 1024, cb),
    (_, cb) => fs.write(fd, Buffer.alloc(4096), 0, 4096, 6 * 1024 * 1024 * 1024, cb),
    (_, cb) => fs.fstat(fd, cb),
    checkSize(6 * 1024 * 1024 * 1024 + 4096),
    (_, cb) => fs.close(fd, cb),
    open('a+'),
    (_, cb) => fs.read(fd, Buffer.alloc(4096), 0, 4096, 0, cb),
    (_, cb) => fs.read(fd, Buffer.alloc(4096), 0, 4096, 4 * 1024 * 1024 * 1024, cb),
    (_, cb) => fs.read(fd, Buffer.alloc(4096), 0, 4096, 6 * 1024 * 1024 * 1024, cb),
    (_, cb) => fs.close(fd, cb),
    (_, cb) => unmount(fuse, cb),
    () => {
      t.same(writes.length, 0)
      t.same(reads.length, 0)
      t.end()
    }
  )

  function open (mode) {
    return (_, cb) => {
      fs.open(path.join(mnt, 'test'), mode, function (_, res) {
        fd = res
        cb()
      })
    }
  }

  function checkSize (n) {
    return ({ size}, cb) => {
      t.same(size, n)
      cb()
    }
  }

  function run (...fns) {
    const all = [...fns]
    tick()
    function tick (err, val) {
      t.error(err, 'no error')
      const next = all.shift()
      if (next) next(val, tick)
    }
  }
})

const tape = require('tape')
const fs = require('fs')
const path = require('path')

const Fuse = require('../')
const createMountpoint = require('./fixtures/mnt')
const stat = require('./fixtures/stat')
const { unmount } = require('./helpers')

const mnt = createMountpoint()

tape('write', function (t) {
  var created = false
  var data = Buffer.alloc(1024)
  var size = 0

  var ops = {
    force: true,
    readdir: function (path, cb) {
      if (path === '/') return process.nextTick(cb, null, created ? ['hello'] : [], [])
      return process.nextTick(cb, Fuse.ENOENT)
    },
    truncate: function (path, size, cb) {
      process.nextTick(cb, 0)
    },
    getattr: function (path, cb) {
      if (path === '/') return process.nextTick(cb, null, stat({ mode: 'dir', size: 4096 }))
      if (path === '/hello' && created) return process.nextTick(cb, 0, stat({ mode: 'file', size: size }))
      return process.nextTick(cb, Fuse.ENOENT)
    },
    create: function (path, flags, cb) {
      t.ok(!created, 'file not created yet')
      created = true
      process.nextTick(cb, 0, 42)
    },
    release: function (path, fd, cb) {
      process.nextTick(cb, 0)
    },
    write: function (path, fd, buf, len, pos, cb) {
      buf.slice(0, len).copy(data, pos)
      size = Math.max(pos + len, size)
      process.nextTick(cb, buf.length)
    }
  }

  const fuse = new Fuse(mnt, ops, { debug: true })
  fuse.mount(function (err) {
    t.error(err, 'no error')

    fs.writeFile(path.join(mnt, 'hello'), 'hello world', function (err) {
      t.error(err, 'no error')
      t.same(data.slice(0, size), Buffer.from('hello world'), 'data was written')

      unmount(fuse, function () {
        t.end()
      })
    })
  })
})

const tape = require('tape')
const fs = require('fs')
const path = require('path')
const concat = require('concat-stream')

const Fuse = require('../')
const mnt = require('./fixtures/mnt')
const stat = require('./fixtures/stat')

tape('read', function (t) {
  var ops = {
    force: true,
    readdir: function (path, cb) {
      if (path === '/') return process.nextTick(cb, null, ['test'])
      return process.nextTick(cb, Fuse.ENOENT)
    },
    getattr: function (path, cb) {
      if (path === '/') return process.nextTick(cb, null, stat({ mode: 'dir', size: 4096 }))
      if (path === '/test') return process.nextTick(cb, null, stat({ mode: 'file', size: 11 }))
      return process.nextTick(cb, Fuse.ENOENT)
    },
    open: function (path, flags, cb) {
      return process.nextTick(cb, 0, 42)
    },
    release: function (path, fd, cb) {
      console.log('IN JS RELEASE')
      t.same(fd, 42, 'fd was passed to release')
      return process.nextTick(cb, 0)
    },
    read: function (path, fd, buf, len, pos, cb) {
      var str = 'hello world'.slice(pos, pos + len)
      if (!str) return process.nextTick(cb, 0)
      buf.write(str)
      return process.nextTick(cb, str.length)
    }
  }

  const fuse = new Fuse(mnt, ops, { debug: true })
  fuse.mount(function (err) {
    t.error(err, 'no error')

    fs.readFile(path.join(mnt, 'test'), function (err, buf) {
      t.error(err, 'no error')
      t.same(buf, Buffer.from('hello world'), 'read file')

      fs.readFile(path.join(mnt, 'test'), function (err, buf) {
        t.error(err, 'no error')
        t.same(buf, Buffer.from('hello world'), 'read file again')

        fs.createReadStream(path.join(mnt, 'test'), { start: 0, end: 4 }).pipe(concat(function (buf) {
          t.same(buf, Buffer.from('hello'), 'partial read file')

          fs.createReadStream(path.join(mnt, 'test'), { start: 6, end: 10 }).pipe(concat(function (buf) {
            t.same(buf, Buffer.from('world'), 'partial read file + start offset')

            fuse.unmount(function () {
              t.end()
            })
          }))
        }))
      })
    })
  })
})

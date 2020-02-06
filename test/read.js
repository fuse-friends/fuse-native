const tape = require('tape')
const fs = require('fs')
const path = require('path')
const concat = require('concat-stream')

const Fuse = require('../')
const createMountpoint = require('./fixtures/mnt')
const stat = require('./fixtures/stat')
const simpleFS = require('./fixtures/simple-fs')

const { unmount } = require('./helpers')
const mnt = createMountpoint()

tape('read', function (t) {
  const testFS = simpleFS({
    release: function (path, fd) {
      t.same(fd, 42, 'fd was passed to release')
    }
  })
  const fuse = new Fuse(mnt, testFS, { debug: true })
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

            unmount(fuse, function () {
              t.end()
            })
          }))
        }))
      })
    })
  })
})

// Skipped because this test takes 2 minutes to run.
tape.skip('read timeout does not force unmount', function (t) {
  var ops = {
    force: true,
    readdir: function (path, cb) {
      if (path === '/') return process.nextTick(cb, null, ['test'])
      return process.nextTick(cb, Fuse.ENOENT)
    },
    getattr: function (path, cb) {
      if (path === '/') return process.nextTick(cb, null, stat({ mode: 'dir', size: 4096 }))
      if (path === '/test') return process.nextTick(cb, null, stat({ mode: 'file', size: 11 }))
      if (path === '/timeout') return process.nextTick(cb, null, stat({ mode: 'file', size: 11 }))
      return process.nextTick(cb, Fuse.ENOENT)
    },
    open: function (path, flags, cb) {
      return process.nextTick(cb, 0, 42)
    },
    release: function (path, fd, cb) {
      t.same(fd, 42, 'fd was passed to release')
      return process.nextTick(cb, 0)
    },
    read: function (path, fd, buf, len, pos, cb) {
      if (path === '/test') {
        var str = 'hello world'.slice(pos, pos + len)
        if (!str) return process.nextTick(cb, 0)
        buf.write(str)
        return process.nextTick(cb, str.length)
      } else if (path === '/timeout') {
        console.log('read is gonna time out')
        // Just let this one timeout
        setTimeout(cb, 20 * 1000, -2)
        return
      }
      return cb(-2)
    }
  }

  const fuse = new Fuse(mnt, ops, { debug: false })
  fuse.mount(function (err) {
    t.error(err, 'no error')

    fs.readFile(path.join(mnt, 'test'), function (err, buf) {
      t.error(err, 'no error')
      t.same(buf, Buffer.from('hello world'), 'read file')

      // Start the read that will time out, wait a bit, then ensure that the second read works.
      console.time('timeout')
      fs.readFile(path.join(mnt, 'timeout'), function (err, buf) {
        console.timeEnd('timeout')
        console.log('the read timed out')
        t.true(err)
      })

      // The default FUSE timeout is 2 minutes, so wait another second after the timeout.
      setTimeout(function () {
        console.log('reading from test')
        fs.readFile(path.join(mnt, 'test'), function (err, buf) {
          t.error(err, 'no error')
          t.same(buf, Buffer.from('hello world'), 'read file')
          unmount(fuse, function () {
            t.end()
          })
        })
      }, 1000 * 121)
    })
  })
})



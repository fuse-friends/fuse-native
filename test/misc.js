const mnt = require('./fixtures/mnt')
const tape = require('tape')

const Fuse = require('../')
const { unmount } = require('./helpers')

tape('mount', function (t) {
  const fuse = new Fuse(mnt, {}, { force: true })
  fuse.mount(function (err) {
    t.error(err, 'no error')
    t.ok(true, 'works')
    unmount(fuse, function () {
      t.end()
    })
  })
})

tape('mount + unmount + mount', function (t) {
  const fuse1 = new Fuse(mnt, {}, { force: true, debug: false })
  const fuse2 = new Fuse(mnt, {}, { force: true, debug: false })

  fuse1.mount(function (err) {
    t.error(err, 'no error')
    t.ok(true, 'works')
    unmount(fuse1, function () {
      fuse2.mount(function (err) {
        t.error(err, 'no error')
        t.ok(true, 'works')
        unmount(fuse2, function () {
          t.end()
        })
      })
    })
  })
})

tape('mount + unmount + mount with same instance fails', function (t) {
  const fuse = new Fuse(mnt, {}, { force: true, debug: false })

  fuse.mount(function (err) {
    t.error(err, 'no error')
    t.ok(true, 'works')
    unmount(fuse, function () {
      fuse.mount(function (err) {
        t.ok(err, 'had error')
        t.end()
      })
    })
  })
})

tape('mnt point must exist', function (t) {
  const fuse = new Fuse('.does-not-exist', {}, { debug: false })
  fuse.mount(function (err) {
    t.ok(err, 'had error')
    t.end()
  })
})

tape('mnt point must be directory', function (t) {
  const fuse = new Fuse(__filename, {}, { debug: false })
  fuse.mount(function (err) {
    t.ok(err, 'had error')
    t.end()
  })
})

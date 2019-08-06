var mnt = require('./fixtures/mnt')
var tape = require('tape')

var Fuse = require('../')

tape('mount', function (t) {
  const fuse = new Fuse(mnt, {}, { force: true })
  fuse.mount(function (err) {
    t.error(err, 'no error')
    t.ok(true, 'works')
    fuse.unmount(function () {
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
    fuse1.unmount(function () {
      fuse2.mount(function (err) {
        t.error(err, 'no error')
        t.ok(true, 'works')
        fuse2.unmount(function () {
          t.end()
        })
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

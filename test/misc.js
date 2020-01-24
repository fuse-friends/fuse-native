const mnt = require('./fixtures/mnt')
const tape = require('tape')
const { spawnSync } = require('child_process')

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
    t.pass('works')
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

tape('mounting twice without force fails', function (t) {
  const fuse1 = new Fuse(mnt, {}, { force: true, debug: false })
  const fuse2 = new Fuse(mnt, {}, { force: false, debug: false })

  fuse1.mount(function (err) {
    t.error(err, 'no error')
    t.pass('works')
    fuse2.mount(function (err) {
      t.true(err, 'cannot mount over existing mountpoint')
      unmount(fuse1, function () {
        t.end()
      })
    })
  })
})

tape('mounting twice with force fail if mountpoint is not broken', function (t) {
  const fuse1 = new Fuse(mnt, {}, { force: true, debug: false })
  const fuse2 = new Fuse(mnt, {}, { force: true, debug: false })

  fuse1.mount(function (err) {
    t.error(err, 'no error')
    t.pass('works')
    fuse2.mount(function (err) {
      t.true(err, 'cannot mount over existing mountpoint')
      unmount(fuse1, function () {
        t.end()
      })
    })
  })
})

tape('mounting over a broken mountpoint with force succeeds', function (t) {
  createBrokenMountpoint(mnt)

  const fuse = new Fuse(mnt, {}, { force: true, debug: false })
  fuse.mount(function (err) {
    t.error(err, 'no error')
    t.pass('works')
    unmount(fuse, function (err) {
      t.end()
    })
  })
})

tape('static unmounting', function (t) {
  t.end()
})

function createBrokenMountpoint (mnt) {
  spawnSync(process.execPath, ['-e', `
    const Fuse = require('..')
    const mnt = ${JSON.stringify(mnt)}
    const fuse = new Fuse(mnt, {}, { force: true, debug: false })
    fuse.mount(() => {
      process.exit(0)
    })
  `], {
    cwd: __dirname,
    stdio: 'inherit'
  })
}

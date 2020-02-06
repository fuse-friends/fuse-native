const os = require('os')
const fs = require('fs')
const tape = require('tape')
const { spawnSync, exec } = require('child_process')

const createMountpoint = require('./fixtures/mnt')

const Fuse = require('../')
const { unmount } = require('./helpers')
const simpleFS = require('./fixtures/simple-fs')

const mnt = createMountpoint()

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

tape('mounting without mkdir option and a nonexistent mountpoint fails', function (t) {
  const nonexistentMnt = createMountpoint({ doNotCreate: true })

  const fuse = new Fuse(nonexistentMnt, {}, { debug: false })
  fuse.mount(function (err) {
    t.true(err, 'could not mount')
    t.end()
  })
})

tape('mounting with mkdir option and a nonexistent mountpoint succeeds', function (t) {
  const nonexistentMnt = createMountpoint({ doNotCreate: true })

  const fuse = new Fuse(nonexistentMnt, {}, { debug: false, mkdir: true })
  fuse.mount(function (err) {
    t.error(err, 'no error')
    unmount(fuse, function (err) {
      t.end()
    })
  })
})

tape('(osx only) unmount with Finder open succeeds', function (t) {
  if (os.platform() !== 'darwin') return t.end()
  const fuse = new Fuse(mnt, simpleFS(), { force: true, debug: false })
  fuse.mount(function (err) {
    t.error(err, 'no error')
    exec(`open ${mnt}`, err => {
      t.error(err, 'no error')
      setTimeout(() => {
        fs.readdir(mnt, (err, list) => {
          t.error(err, 'no error')
          t.same(list, ['test'])
          unmount(fuse, err => {
            t.error(err, 'no error')
            fs.readdir(mnt, (err, list) => {
              t.error(err, 'no error')
              t.same(list, [])
              t.end()
            })
          })
        })
      }, 1000)
    })
  })
})

tape('(osx only) unmount with Terminal open succeeds', function (t) {
  if (os.platform() !== 'darwin') return t.end()
  const fuse = new Fuse(mnt, simpleFS(), { force: true, debug: false })
  fuse.mount(function (err) {
    t.error(err, 'no error')
    exec(`open -a Terminal ${mnt}`, err => {
      t.error(err, 'no error')
      setTimeout(() => {
        fs.readdir(mnt, (err, list) => {
          t.error(err, 'no error')
          t.same(list, ['test'])
          unmount(fuse, err => {
            t.error(err, 'no error')
            fs.readdir(mnt, (err, list) => {
              t.error(err, 'no error')
              t.same(list, [])
              t.end()
            })
          })
        })
      }, 1000)
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

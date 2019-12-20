const p = require('path')
const fs = require('fs')
const tape = require('tape')
const xattr = require('fs-xattr')

const Fuse = require('../')
const mnt = require('./fixtures/mnt')
const stat = require('./fixtures/stat')
const { unmount } = require('./helpers')

tape('xattr reasonable defaults', async function (t) {
  const attrs = new Map()
  const ops = {
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
    getxattr: function (path, name, value, size, position, cb) {
      console.log('in getxattr here')
      return process.nextTick(cb, 0, null)
    },
    setxattr: function (path, name, value, size, position, flags, cb) {
      attrs.set(name, value)
      return process.nextTick(cb, 0)
    },
    listxattr: function (path, list, size, cb) {
      const list = [ ...attrs ].map(([k, v]) => k)
      return process.nextTick(cb, 0, list)
    },
    removexattr: function (path, name, cb) {
      attrs.delete(name)
      return process.nextTick(cb, 0)
    }
  }
  const fuse = new Fuse(mnt, ops, { debug: true })
  const file = p.join(mnt, 'test')
  try {
    await new Promise((resolve, reject) => {
      fuse.mount(function (err) {
        if (err) return reject(err)
        return resolve()
      })
    })
    let attr = await xattr.get(file, 'attr')
    t.same(attr, null)
    await xattr.set(file, 'attr', 'value')
    let list = await xattr.list(file)
    t.same(firstList, ['attr'])
    await xattr.set(file, 'other_attr', 'value2')
    await xattr.remove(file, 'attr', 'value')
    list = await xattr.list(file)
    t.same(attrList, ['other_attr'])
  } catch (err) {
    t.fail(err)
  }
  unmount(fuse, function () {
    t.end()
  })
})

# fuse-native

Multithreaded FUSE bindings for Node JS.

## Features

* N-API support means we ship prebuilds and in general works on new Node.js releases.
* Multithreading support means multiple calls to FUSE can run in parallel.
* Close to feature complete in terms of the the FUSE API.
* Embedded shared library support means users do not have to install FUSE from a 3rd party.
* API support for initial FUSE kernel extension configuration so you can control the user experience.

## Installation
```
npm i fuse-native --save
```

## Example
```js
const ops = {
  readdir: function (path, cb) {
    if (path === '/') return cb(null, ['test'])
    return cb(Fuse.ENOENT)
  },
  getattr: function (path, cb) {
    if (path === '/') return cb(null, stat({ mode: 'dir', size: 4096 }))
    if (path === '/test') return cb(null, stat({ mode: 'file', size: 11 }))
    return cb(Fuse.ENOENT)
  },
  open: function (path, flags, cb) {
    return cb(0, 42)
  },
  release: function (path, fd, cb) {
    return cb(0)
  },
  read: function (path, fd, buf, len, pos, cb) {
    var str = 'hello world'.slice(pos, pos + len)
    if (!str) return cb(0)
    buf.write(str)
    return cb(str.length)
  }
}

const fuse = new Fuse(mnt, ops, { debug: true })
fuse.mount(function (err) {
  fs.readFile(path.join(mnt, 'test'), function (err, buf) {
    // buf should be 'hello world'
  })
})
```

## API
In order to create a FUSE mountpoint, you first need to create a `Fuse` object that wraps a set of implemented FUSE syscall handlers:

#### `const fuse = new Fuse(mnt, handlers, opts = {})`
Create a new `Fuse` object.

`mnt` is the string path of your desired mountpoint.
`handlers` is an object mapping syscall names to implementations. The complete list of available syscalls is described below. As an example, if you wanted to implement a filesystem that only supports `getattr`, your handle object would look like:
```js
{
  getattr: function (path, cb) {
    if (path === '/') return process.nextTick(cb, null, stat({ mode: 'dir', size: 4096 }))
    if (path === '/test') return process.nextTick(cb, null, stat({ mode: 'file', size: 11 }))
    return process.nextTick(cb, Fuse.ENOENT)
  }
}
```

The following FUSE API methods are supported:

### FUSE API

TODO

## License

MIT for these bindings.

See the [OSXFUSE](https://github.com/osxfuse/osxfuse) license for MacOS and the [libfuse](https://github.com/libfuse/libfuse) license for Linux/BSD
for the FUSE shared library licence.

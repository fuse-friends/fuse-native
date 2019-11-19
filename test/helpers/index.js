exports.unmount = function (fuse, cb) { // This only seems to be nessesary an the ancient osx we use on travis so ... yolo
  fuse.unmount(function (err) {
    if (err) return cb(err)
    setTimeout(cb, 1000)
  })
}

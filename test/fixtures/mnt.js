var os = require('os')
var path = require('path')
var fs = require('fs')

function create (opts = {}) {
  var mnt = path.join(os.tmpdir(), 'fuse-bindings-' + process.pid + '-' + Date.now())

  if (!opts.doNotCreate) {
    try {
      fs.mkdirSync(mnt)
    } catch (err) {
      // do nothing
    }
  }

  return mnt
}

module.exports = create

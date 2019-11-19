#!/usr/bin/env node

const Fuse = require('./')
const cmd = process.argv[2]

if (cmd === 'configure') {
  Fuse.configure(onerror)
} else if (cmd === 'unconfigure') {
  Fuse.unconfigure(onerror)
} else if (cmd === 'is-configured') {
  Fuse.isConfigured(function (err, bool) {
    if (err) return onerror(err)
    console.log('' + bool)
    process.exit(bool ? 0 : 1)
  })
}

function onerror (err) {
  if (err) throw err
}

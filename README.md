# fuse-native

Multithreaded FUSE bindings for Node JS.

## Features

* N-API support means we ship prebuilds and in general works on new Node.js releases.
* Multithreading support means multiple calls to FUSE can run in parallel.
* Close to feature complete in terms of the the FUSE API.
* Embedded shared library support means users do not have to install FUSE from a 3rd party.
* API support for initial FUSE kernel extension configuration so you can control the user experience.

## API

TODO

## License

MIT for these bindings.

See the [OSXFUSE](https://github.com/osxfuse/osxfuse) license for MacOS and the [libfuse](https://github.com/libfuse/libfuse) license for Linux/BSD
for the FUSE shared library licence.

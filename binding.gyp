{
  "targets": [{
    "target_name": "fuse",
    "include_dirs": [
      "<!(node -e \"require('napi-macros')\")",
      "<!(node -e \"require('fuse-shared-library/include')\")",
    ],
    "libraries": [
      "<!(node -e \"require('fuse-shared-library/lib')\")",
    ],
    "sources": [
      "fuse-native.c"
    ],
    'xcode_settings': {
      'OTHER_CFLAGS': [
        '-g',
        '-O3',
        '-Wall'
      ]
    },
    'cflags': [
      '-g',
      '-O3',
      '-Wall'
    ],
  }, {
    "target_name": "postinstall",
    "type": "none",
    "dependencies": ["fuse"],
    "copies": [{
      "destination": "build/Release",
      "files": [ "<!(node -e \"require('fuse-shared-library/lib')\")" ],
    }]
  }]
}

{
  "targets": [{
    "target_name": "fuse",
    "include_dirs": [
      "<!(node -e \"require('napi-macros')\")",
      "<!(node -e \"require('nan')\")",
      "<!(node -e \"require('fuse-shared-library/include')\")",
    ],
    "libraries": [
      "<!(node -e \"require('fuse-shared-library/lib')\")",
    ],
    "sources": [
      "fuse-bindings.cc",
      "abstractions.cc"
    ]
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

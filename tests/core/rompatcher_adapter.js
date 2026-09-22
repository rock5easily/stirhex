#!/usr/bin/env node

// Internal API adapter for RomPatcher.js interoperability tests.
// The v3.2.1 CLI option is named --validate-checksum but is not wired to the
// library's requireValidation option. This adapter calls applyPatch directly
// with requireValidation:true so source/target CRC checks are real.

const fs = require('fs');
const path = require('path');

function usage() {
  console.error('usage: node rompatcher_adapter.js --root ROMPATCHER_ROOT apply SOURCE PATCH OUTPUT [--validate]');
  console.error('       node rompatcher_adapter.js --root ROMPATCHER_ROOT create SOURCE TARGET FORMAT OUTPUT');
}

function fail(message) {
  console.error(`error: ${message}`);
  process.exitCode = 1;
}

const argv = process.argv.slice(2);
if (argv.length < 2 || argv[0] !== '--root') {
  usage();
  process.exit(2);
}

const root = path.resolve(argv[1]);
const BinFile = require(path.join(root, 'rom-patcher-js', 'modules', 'BinFile'));
const RomPatcher = require(path.join(root, 'rom-patcher-js', 'RomPatcher'));
const operation = argv[2];

try {
  if (operation === 'apply' && (argv.length === 6 || argv.length === 7)) {
    const sourcePath = path.resolve(argv[3]);
    const patchPath = path.resolve(argv[4]);
    const outputPath = path.resolve(argv[5]);
    const validate = argv[6] === '--validate';
    const source = new BinFile(sourcePath);
    const patchFile = new BinFile(patchPath);
    const patch = RomPatcher.parsePatchFile(patchFile);
    if (!patch) throw new Error('Invalid patch file');
    const output = RomPatcher.applyPatch(source, patch, {
      requireValidation: validate,
      outputSuffix: false,
    });
    output.fileName = outputPath;
    output.save();
    console.log(JSON.stringify({ ok: true, operation, validate, output: outputPath }));
    process.exit(0);
  }
  if (operation === 'create' && argv.length === 7) {
    const source = new BinFile(path.resolve(argv[3]));
    const target = new BinFile(path.resolve(argv[4]));
    const format = argv[5];
    const outputPath = path.resolve(argv[6]);
    const patch = RomPatcher.createPatch(source, target, format);
    const output = patch.export();
    output.fileName = outputPath;
    output.save();
    console.log(JSON.stringify({ ok: true, operation, format, output: outputPath }));
    process.exit(0);
  }
  usage();
  process.exit(2);
} catch (error) {
  fail(error && error.message ? error.message : String(error));
}

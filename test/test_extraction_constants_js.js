#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

const fs = require('fs');
const vm = require('vm');

const [constantsPath] = process.argv.slice(2);
if (!constantsPath) {
  console.error('usage: test_extraction_constants_js.js CONSTANTS_JS');
  process.exit(2);
}

const context = {};
vm.createContext(context);
vm.runInContext(
  fs.readFileSync(constantsPath, 'utf8') +
    '\nglobalThis.eventKindLabel = eventKindLabel;',
  context,
);

if (context.eventKindLabel(6, 3) !== 'PUMP_OFF') {
  throw new Error('v6 pump-off label changed');
}
if (context.eventKindLabel(7, 3) !== 'PUMP_OFF_CONFIRMED') {
  throw new Error('v7 pump-off label changed');
}
if (context.eventKindLabel(7, 99) !== 99) {
  throw new Error('unknown event kind did not retain its numeric value');
}

console.log('OK: extraction event labels preserve version semantics');

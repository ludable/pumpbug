#!/usr/bin/env node
// SPDX-FileCopyrightText: 2026 ludable
// SPDX-License-Identifier: AGPL-3.0-only

const fs = require('fs');
const vm = require('vm');

const [protocolPath] = process.argv.slice(2);
if (!protocolPath) {
  console.error('usage: test_extraction_protocol_js.js PROTOCOL_JS');
  process.exit(2);
}

const CURRENT_VERSION = 7;
const OLDEST_VERSION = 6;
const HEADER_BYTES = 54;
const TARGET_BLOCK_BYTES = 20;

const context = {
  DataView,
  NO_WEIGHT_CG: -32768,
  RECORD_FLAG_SCALE_TIMERS: 2,
  SCALE_TIMER_UNKNOWN_MS: 0xffffffff,
};
vm.createContext(context);
vm.runInContext(
  fs.readFileSync(protocolPath, 'utf8') +
    '\nglobalThis.parseExtraction = parseExtraction;',
  context,
);

function uvarint(value) {
  const out = [];
  value >>>= 0;
  while (value >= 0x80) {
    out.push((value & 0x7f) | 0x80);
    value >>>= 7;
  }
  out.push(value);
  return out;
}

function record(version, events = [], lastPumpOffConfirmedMs = 0,
                beginMs = 1000) {
  const header = new Uint8Array(HEADER_BYTES);
  const view = new DataView(header.buffer);
  header.set([0x45, 0x58, 0x54, 0x52, version]);
  view.setUint32(8, beginMs, true);
  view.setUint32(12, lastPumpOffConfirmedMs, true);
  view.setUint16(40, events.length, true);

  const eventBytes = [];
  let previousMs = beginMs;
  for (const event of events) {
    eventBytes.push(...uvarint(event.tMs - previousMs), event.kind);
    if (version >= 7 && event.kind === 3) {
      eventBytes.push(...uvarint(event.signalDecayLeadMs == null
        ? 0 : event.signalDecayLeadMs + 1));
    }
    previousMs = event.tMs;
  }

  const bytes = new Uint8Array(
    HEADER_BYTES + eventBytes.length + TARGET_BLOCK_BYTES);
  bytes.set(header);
  bytes.set(eventBytes, HEADER_BYTES);
  return bytes;
}

const decoded = context.parseExtraction(record(CURRENT_VERSION).buffer);
if (decoded.version !== CURRENT_VERSION) {
  throw new Error('current-version record did not decode');
}

const legacyBytes = record(OLDEST_VERSION);
legacyBytes[45] = 99;
const legacy = context.parseExtraction(legacyBytes.buffer);
if (legacy.version !== OLDEST_VERSION) {
  throw new Error('legacy record did not retain its version');
}

for (const version of [OLDEST_VERSION - 1, CURRENT_VERSION + 1]) {
  let rejected = false;
  try {
    context.parseExtraction(record(version).buffer);
  } catch (error) {
    rejected = new RegExp(`unsupported version ${version}`).test(String(error));
  }
  if (!rejected) throw new Error(`version ${version} was not rejected`);
}

const pumpOff = context.parseExtraction(record(CURRENT_VERSION, [
  { tMs: 1000, kind: 2 },
  { tMs: 9500, kind: 3, signalDecayLeadMs: 1500 },
], 9500).buffer);
if (pumpOff.lastPumpOffConfirmedMs !== 9500 ||
    pumpOff.events[1].signalDecayLeadMs !== 1500 ||
    pumpOff.events[1].signalDecayOnsetMs !== 8000) {
  throw new Error('v7 pump-off payload did not decode');
}

const confirmationOnly = context.parseExtraction(record(CURRENT_VERSION, [
  { tMs: 9500, kind: 3 },
], 9500).buffer);
if ('signalDecayOnsetMs' in confirmationOnly.events[0]) {
  throw new Error('zero pump-off payload did not remain absent');
}

const legacyPumpOff = context.parseExtraction(record(OLDEST_VERSION, [
  { tMs: 9500, kind: 3 },
], 9500).buffer);
if ('signalDecayOnsetMs' in legacyPumpOff.events[0]) {
  throw new Error('v6 pump-off acquired v7 timing semantics');
}

const rolloverPumpOff = context.parseExtraction(record(CURRENT_VERSION, [
  { tMs: 100, kind: 3, signalDecayLeadMs: 117 },
], 100, 0xffffffff - 200).buffer);
if (rolloverPumpOff.events[0].signalDecayOnsetMs !== 0xffffffff - 16) {
  throw new Error('millis rollover pump-off payload did not decode');
}

let oversizedPayloadRejected = false;
try {
  context.parseExtraction(record(CURRENT_VERSION, [
    { tMs: 9500, kind: 3, signalDecayLeadMs: 0xffff },
  ], 9500).buffer);
} catch (error) {
  oversizedPayloadRejected = /invalid event payload/.test(String(error));
}
if (!oversizedPayloadRejected) {
  throw new Error('oversized v7 pump-off payload was not rejected');
}

console.log('OK: extraction protocol versions and pump-off payload');

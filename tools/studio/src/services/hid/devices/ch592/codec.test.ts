import { describe, expect, it } from 'vitest';
import { Ch592Codec } from './codec';
import { createDefaultRgbConfig } from '@/types/protocol';

function rgbResponse(payloadLength: number, seamlessWake = 1, deepSeamlessWake2g4 = 1): DataView {
  const bytes = new Uint8Array(64);
  bytes[2] = payloadLength;
  bytes[3] = 0;
  bytes[4] = 1;
  bytes[5] = 5;
  bytes[6] = 64;
  bytes[7] = 128;
  bytes[8] = 1;
  bytes[9] = 2;
  bytes[10] = 3;
  bytes[11] = 1;
  bytes[12] = 64;
  bytes[13] = 0;
  bytes[14] = 1;
  bytes[15] = 1;
  bytes[16] = seamlessWake;
  bytes[17] = deepSeamlessWake2g4;
  return new DataView(bytes.buffer);
}

describe('CH592 seamless wake RGB protocol extension', () => {
  it('reads and writes the 13th setting byte with new firmware', () => {
    const codec = new Ch592Codec();
    const config = codec.parseRgbConfig(rgbResponse(14, 0));

    expect(config.seamlessWakeEnabled).toBe(false);
    config.seamlessWakeEnabled = true;
    const payload = codec.buildSetRgbPayload(config);
    expect(payload).toHaveLength(13);
    expect(payload[12]).toBe(1);
  });

  it('reads and writes the optional 2.4G DEEP wake byte', () => {
    const codec = new Ch592Codec();
    const config = codec.parseRgbConfig(rgbResponse(15, 1, 0));

    expect(config.deepSeamlessWake2g4Enabled).toBe(false);
    config.deepSeamlessWake2g4Enabled = true;
    const payload = codec.buildSetRgbPayload(config);
    expect(payload).toHaveLength(14);
    expect(payload[13]).toBe(1);
  });

  it('keeps the legacy 12-byte request for old firmware', () => {
    const codec = new Ch592Codec();
    const config = codec.parseRgbConfig(rgbResponse(13));

    expect(config.seamlessWakeEnabled).toBe(true);
    expect(config.deepSeamlessWake2g4Enabled).toBeUndefined();
    config.seamlessWakeEnabled = false;
    expect(codec.buildSetRgbPayload(config)).toHaveLength(12);
  });

  it('defaults seamless wake to enabled before connecting', () => {
    expect(createDefaultRgbConfig().seamlessWakeEnabled).toBe(true);
  });
});

describe('CH592 receiver system status', () => {
  it('decodes the startup stage after SYS_INFO identifies a receiver', () => {
    const codec = new Ch592Codec();
    const info = new Uint8Array(64);
    info[2] = 18;
    info[3] = 0;
    info[18] = 1;
    codec.parseSysInfo(new DataView(info.buffer));
    const bytes = new Uint8Array(64);
    // Command, sub, payload length, OK, work mode, link state, layer,
    // battery, charging, receiver startup stage, reserved, reserved.
    bytes.set([0x02, 0x00, 0x09, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00]);

    const status = codec.parseSysStatus(new DataView(bytes.buffer));

    expect(status.workMode).toBe(2);
    expect(status.receiverStartupStage).toBe(1);
    expect(status.receiverHostStartupState).toBe(0);
    expect(status.receiverHostStartupResult).toBe(0);
    expect(status.adcRaw).toBeUndefined();
  });
});

describe('CH592 keymap response safety', () => {
  it('decodes all eight actions without shifting the response header', () => {
    const codec = new Ch592Codec();
    const bytes = new Uint8Array(64);
    bytes.set([0x20, 0x00, 0x24, 0x00, 0x05, 0x00, 0x00], 0);
    // K1 = keyboard A, K2 = layer toggle to layer 2, remaining actions kept.
    bytes.set([0x01, 0x00, 0x04, 0x00], 7);
    bytes.set([0x06, 0x01, 0x01, 0x00], 11);
    const parsed = codec.parseKeymap(new DataView(bytes.buffer));
    expect(parsed.numLayers).toBe(5);
    expect(parsed.layer.keys[0]).toEqual({ type: 1, modifier: 0, param1: 4, param2: 0 });
    expect(parsed.layer.keys[1]).toEqual({ type: 6, modifier: 1, param1: 1, param2: 0 });
  });

  it('rejects a short response instead of creating an empty keymap', () => {
    const codec = new Ch592Codec();
    const bytes = new Uint8Array([0x20, 0x00, 0x04, 0x00, 0x01, 0x00, 0x00]);
    expect(() => codec.parseKeymap(new DataView(bytes.buffer))).toThrow(/返回长度错误/);
  });
});

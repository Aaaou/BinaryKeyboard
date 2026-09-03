import { afterEach, describe, expect, it, vi } from 'vitest';

import { resolveFirmwareUrls } from '@/services/iapService';

const asset = {
  channel: 'release',
  version: '9.9.9',
  appBinUrl: 'https://example.test/app.bin',
  bleAppBinUrl: 'https://example.test/ble.bin',
  radio2g4AppBinUrl: 'https://example.test/radio.bin',
};

function mockManifest() {
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue({
    ok: true,
    json: async () => ({ artifacts: { ch592: { KNOB: asset } } }),
  }));
}

describe('trimode firmware selection', () => {
  afterEach(() => vi.unstubAllGlobals());

  it('updates 2.4G first and USB/BLE last when currently in USB/BLE', async () => {
    mockManifest();
    await expect(resolveFirmwareUrls('9.9.9', 'KNOB', true, 0)).resolves.toEqual([
      asset.radio2g4AppBinUrl,
      asset.bleAppBinUrl,
    ]);
  });

  it('updates USB/BLE first and 2.4G last when currently in 2.4G', async () => {
    mockManifest();
    await expect(resolveFirmwareUrls('9.9.9', 'KNOB', true, 2)).resolves.toEqual([
      asset.bleAppBinUrl,
      asset.radio2g4AppBinUrl,
    ]);
  });

  it('keeps the historical single-image OTA asset for non-trimode firmware', async () => {
    mockManifest();
    await expect(resolveFirmwareUrls('9.9.9', 'KNOB', false, 0)).resolves.toEqual([
      asset.appBinUrl,
    ]);
  });
});

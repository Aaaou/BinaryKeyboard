import { describe, expect, it } from 'vitest';

import { Command, ResponseCode } from '@/types/protocol';
import { parseReceiveFrame } from './protocolParser';

function batteryFrame(data: number[]): Uint8Array {
  const frame = new Uint8Array(64);
  frame[0] = Command.BATTERY;
  frame[1] = 0;
  frame[2] = data.length;
  frame.set(data, 3);
  return frame;
}

describe('BATTERY response parsing', () => {
  it('appends raw ADC and CHRG pin values from diagnostic firmware', () => {
    const frame = batteryFrame([
      ResponseCode.OK,
      5,
      0,
      0xe4,
      0x0c,
      0x49,
      0x06,
      1,
    ]);

    expect(parseReceiveFrame(frame).parsed).toContain(
      '5% 未充电 3.30V | ADC原始值 1609 | CHRG原始值 1',
    );
  });

  it('keeps the legacy output for old five-byte responses', () => {
    const frame = batteryFrame([
      ResponseCode.OK,
      5,
      0,
      0xe4,
      0x0c,
    ]);
    const parsed = parseReceiveFrame(frame).parsed;

    expect(parsed).toContain('5% 未充电 3.30V');
    expect(parsed).not.toContain('ADC原始值');
    expect(parsed).not.toContain('CHRG原始值');
  });
});

describe('SYS_STATUS response parsing', () => {
  it('labels receiver stage instead of treating it as BLE battery data', () => {
    const frame = new Uint8Array(64);
    frame[0] = Command.SYS_STATUS;
    frame[2] = 9;
    frame.set([ResponseCode.OK, 2, 0, 0, 0, 0, 1, 0, 0], 3);

    const parsed = parseReceiveFrame(frame).parsed;
    expect(parsed).toContain('2.4G 接收器 未连接');
    expect(parsed).toContain('启动阶段 1');
    expect(parsed).not.toContain('ADC原始值');
  });

  it('maps firmware connection state 2 to BLE connected', () => {
    const frame = new Uint8Array(64);
    frame[0] = Command.SYS_STATUS;
    frame[2] = 6;
    frame.set([
      ResponseCode.OK,
      1,
      2,
      0,
      94,
      1,
    ], 3);

    expect(parseReceiveFrame(frame).parsed).toContain(
      'BLE 已连接 | 层1 | 电量 94% 充电中',
    );
  });

  it('maps firmware connection state 1 to BLE advertising', () => {
    const frame = new Uint8Array(64);
    frame[0] = Command.SYS_STATUS;
    frame[2] = 6;
    frame.set([
      ResponseCode.OK,
      1,
      1,
      0,
      50,
      0,
    ], 3);

    expect(parseReceiveFrame(frame).parsed).toContain('BLE 广播中');
  });

  it('appends raw ADC and CHRG pin values from diagnostic firmware', () => {
    const frame = new Uint8Array(64);
    frame[0] = Command.SYS_STATUS;
    frame[2] = 9;
    frame.set([
      ResponseCode.OK,
      1,
      0,
      4,
      6,
      0,
      0x58,
      0x06,
      1,
    ], 3);

    expect(parseReceiveFrame(frame).parsed).toContain(
      '电量 6% 未充电 | ADC原始值 1624 | CHRG原始值 1',
    );
  });
});

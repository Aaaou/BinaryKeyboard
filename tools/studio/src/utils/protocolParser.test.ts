import { describe, expect, it } from 'vitest';

import { Command, ResponseCode } from '@/types/protocol';
import { parseLogFrame, parseReceiveFrame } from './protocolParser';

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
    const parsed = parseReceiveFrame(frame, { receiverRole: true }).parsed;

    expect(parsed).toContain('5% 未充电 3.30V');
    expect(parsed).not.toContain('ADC原始值');
    expect(parsed).not.toContain('CHRG原始值');
  });

  it('keeps a 2.4G keyboard status distinct from a receiver', () => {
    const frame = new Uint8Array(64);
    frame[0] = Command.SYS_STATUS;
    frame[2] = 9;
    frame.set([ResponseCode.OK, 2, 2, 0, 100, 1, 0xff, 0x0f, 0], 3);

    const parsed = parseReceiveFrame(frame, { receiverRole: false }).parsed;
    expect(parsed).toContain('2.4G 键盘 已连接');
    expect(parsed).toContain('ADC原始值 4095');
    expect(parsed).not.toContain('启动阶段');
  });
});

describe('macro and receiver HID diagnostic logs', () => {
  it('describes a keyboard macro action with its type and parameter', () => {
    const parsed = parseLogFrame(
      new Uint8Array([Command.LOG, 0x07, 0x03, 0x21, 0x02, 0x22]),
    ).parsed;

    expect(parsed).toContain('宏 HID 动作已提交');
    expect(parsed).toContain('动作 0x02');
    expect(parsed).toContain('参数 0x22');
  });

  it('identifies an RF all-zero release submitted directly to USB', () => {
    const parsed = parseLogFrame(
      new Uint8Array([Command.LOG, 0x07, 0x03, 0x9f, 0x34, 0x00]),
    ).parsed;

    expect(parsed).toContain('RF 键盘报告已直接提交 USB');
    expect(parsed).toContain('RF序号低8位 52');
    expect(parsed).toContain('全零释放');
  });

  it('describes a macro action waiting for RF retry', () => {
    const parsed = parseLogFrame(
      new Uint8Array([Command.LOG, 0x07, 0x03, 0x24, 0x02, 0x22]),
    ).parsed;

    expect(parsed).toContain('宏 HID 动作等待重试');
    expect(parsed).toContain('动作 0x02');
    expect(parsed).toContain('参数 0x22');
  });

  it('identifies receiver keyboard lease expiry', () => {
    const parsed = parseLogFrame(
      new Uint8Array([Command.LOG, 0x07, 0x03, 0xa2, 0x03, 0x00]),
    ).parsed;

    expect(parsed).toContain('RF 按键状态租约过期，强制释放');
  });
});

describe('SYS_STATUS response parsing', () => {
  it('labels receiver stage instead of treating it as BLE battery data', () => {
    const frame = new Uint8Array(64);
    frame[0] = Command.SYS_STATUS;
    frame[2] = 9;
    frame.set([ResponseCode.OK, 2, 0, 0, 0, 0, 1, 0, 0], 3);

    const parsed = parseReceiveFrame(frame, { receiverRole: true }).parsed;
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

describe('LAYER_GET sleep diagnostics', () => {
  it('keeps the legacy four-byte response unchanged', () => {
    const frame = new Uint8Array(64);
    frame[0] = Command.LAYER_GET;
    frame[2] = 4;
    frame.set([ResponseCode.OK, 0, 4, 0], 3);

    const parsed = parseReceiveFrame(frame).parsed;
    expect(parsed).toContain('当前层=1, 层数=4, 默认层=1');
    expect(parsed).not.toContain('休眠诊断');
  });

});

describe('RADIO_PAIR_STATUS diagnostics', () => {
  it('prints receiver RF-to-USB HID counters', () => {
    const frame = new Uint8Array(64);
    const data = new Uint8Array(61);
    frame[0] = Command.RADIO_PAIR_STATUS;
    frame[2] = data.length;
    data[0] = ResponseCode.OK;
    data[1] = 3;
    data[2] = 1;
    data[29] = 2;
    data[30] = 3;
    data.set([12, 0, 5, 0, 4, 0, 7, 0, 0], 52);
    frame.set(data, 3);

    const parsed = parseReceiveFrame(frame).parsed;
    expect(parsed).toContain('HID RF有效=12 键盘报告=5');
    expect(parsed).toContain('USB提交=4 USB忙=7 RX隔离=否');
  });

  it('prints keyboard management receive, execute, and response counters', () => {
    const frame = new Uint8Array(64);
    const data = new Uint8Array(61);
    frame[0] = Command.RADIO_PAIR_STATUS;
    frame[2] = data.length;
    data[0] = ResponseCode.OK;
    data[1] = 3;
    data[2] = 0;
    data[29] = 2;
    data[30] = 1;
    data.set([0x14, 2, Command.KEYMAP_GET, 1, 1, 1, 2, 3, 0, 1, 0, 2, 0], 48);
    frame.set(data, 3);

    const parsed = parseReceiveFrame(frame).parsed;
    expect(parsed).toContain('RX分片=1/1 TX分片=1/2');
    expect(parsed).toContain('收到=3 执行=1 响应提交=2');
  });
});

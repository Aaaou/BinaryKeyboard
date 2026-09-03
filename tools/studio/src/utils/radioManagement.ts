/** Wire-compatible management tunnel used by the 2.4G receiver proxy. */
export const RADIO_MGMT_VERSION = 1;
export const RADIO_MGMT_MAX_DATA = 20;
export const RADIO_MGMT_FLAG_FIRST = 0x01;
export const RADIO_MGMT_FLAG_LAST = 0x02;
export const RADIO_MGMT_FLAG_ERROR = 0x04;

export interface RadioManagementFragment {
  transaction: number;
  command: number;
  sub: number;
  fragment: number;
  fragments: number;
  flags: number;
  data: Uint8Array;
}

export function encodeRadioManagementFragment(value: RadioManagementFragment): Uint8Array {
  if (!Number.isInteger(value.fragments) || value.fragments < 1 || value.fragment < 0 || value.fragment >= value.fragments) {
    throw new Error('invalid radio management fragment index');
  }
  if (value.data.length > RADIO_MGMT_MAX_DATA) throw new Error('radio management fragment too large');
  const out = new Uint8Array(8 + value.data.length);
  out.set([RADIO_MGMT_VERSION, value.transaction & 0xff, value.command & 0xff, value.sub & 0xff,
    value.fragment & 0xff, value.fragments & 0xff, value.data.length & 0xff, value.flags & 0xff]);
  out.set(value.data, 8);
  return out;
}

export function decodeRadioManagementFragment(frame: Uint8Array): RadioManagementFragment {
  if (frame.length < 8 || frame[0] !== RADIO_MGMT_VERSION) throw new Error('invalid radio management frame');
  const length = frame[6] ?? 0;
  const fragments = frame[5] ?? 0;
  const fragment = frame[4] ?? 0;
  if (!fragments || fragment >= fragments || length !== frame.length - 8 || length > RADIO_MGMT_MAX_DATA) {
    throw new Error('invalid radio management fragment');
  }
  if (fragment === 0 && !(frame[7]! & RADIO_MGMT_FLAG_FIRST)) throw new Error('missing first flag');
  if (fragment + 1 === fragments && !(frame[7]! & RADIO_MGMT_FLAG_LAST)) throw new Error('missing last flag');
  const expectedFlags = (fragment === 0 ? RADIO_MGMT_FLAG_FIRST : 0) |
    (fragment + 1 === fragments ? RADIO_MGMT_FLAG_LAST : 0);
  const structuralFlags = frame[7]! & (RADIO_MGMT_FLAG_FIRST | RADIO_MGMT_FLAG_LAST);
  if (structuralFlags !== expectedFlags) throw new Error('misplaced management flags');
  if ((frame[7]! & ~(RADIO_MGMT_FLAG_FIRST | RADIO_MGMT_FLAG_LAST | RADIO_MGMT_FLAG_ERROR)) !== 0) {
    throw new Error('unknown management flags');
  }
  return { transaction: frame[1]!, command: frame[2]!, sub: frame[3]!, fragment, fragments, flags: frame[7]!, data: frame.slice(8) };
}

export function splitRadioManagementPayload(transaction: number, command: number, sub: number, data: Uint8Array): Uint8Array[] {
  const count = Math.max(1, Math.ceil(data.length / RADIO_MGMT_MAX_DATA));
  return Array.from({ length: count }, (_, fragment) => encodeRadioManagementFragment({
    transaction, command, sub, fragment, fragments: count,
    flags: (fragment === 0 ? RADIO_MGMT_FLAG_FIRST : 0) | (fragment + 1 === count ? RADIO_MGMT_FLAG_LAST : 0),
    data: data.slice(fragment * RADIO_MGMT_MAX_DATA, (fragment + 1) * RADIO_MGMT_MAX_DATA),
  }));
}

export function joinRadioManagementPayload(fragments: RadioManagementFragment[]): Uint8Array {
  if (!fragments.length) return new Uint8Array();
  const first = fragments[0]!;
  if (fragments.length !== first.fragments) throw new Error('incomplete radio management response');
  const ordered = [...fragments].sort((a, b) => a.fragment - b.fragment);
  ordered.forEach((item, index) => {
    if (item.transaction !== first.transaction || item.command !== first.command || item.sub !== first.sub || item.fragment !== index || item.fragments !== first.fragments) {
      throw new Error('inconsistent radio management response');
    }
  });
  const out = new Uint8Array(ordered.reduce((n, item) => n + item.data.length, 0));
  let offset = 0;
  for (const item of ordered) { out.set(item.data, offset); offset += item.data.length; }
  return out;
}

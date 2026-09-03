import { describe, expect, it } from 'vitest';
import {
  decodeRadioManagementFragment,
  joinRadioManagementPayload,
  splitRadioManagementPayload,
} from './radioManagement';

describe('radio management tunnel', () => {
  it('round-trips a payload across fragments', () => {
    const source = Uint8Array.from({ length: 47 }, (_, i) => i);
    const fragments = splitRadioManagementPayload(7, 0x21, 3, source);
    expect(fragments).toHaveLength(3);
    expect(joinRadioManagementPayload(fragments.map(decodeRadioManagementFragment))).toEqual(source);
  });

  it('rejects missing first/last flags and inconsistent ordering', () => {
    const fragment = splitRadioManagementPayload(1, 2, 0, new Uint8Array([1]))[0]!;
    fragment[7] = 0;
    expect(() => decodeRadioManagementFragment(fragment)).toThrow();
    const parts = splitRadioManagementPayload(1, 2, 0, new Uint8Array(21));
    const decoded = parts.map(decodeRadioManagementFragment);
    decoded[1]!.transaction = 9;
    expect(() => joinRadioManagementPayload(decoded)).toThrow();
  });
});

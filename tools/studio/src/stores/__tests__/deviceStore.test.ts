import { afterEach, beforeEach, describe, expect, it, vi } from "vitest";
import { createPinia, setActivePinia } from "pinia";
import { useDeviceStore } from "@/stores/deviceStore";
import {
  ActionType,
  createDeviceCapabilities,
  createEmptyKeymap,
  DeviceProtocol,
  KeyboardType,
  type DeviceInfo,
  type DeviceStatus,
} from "@/types/protocol";

const mocks = vi.hoisted(() => ({
  hidService: {
    getPairStatus: vi.fn(),
    getSysStatus: vi.fn(),
    getReceiverBootLog: vi.fn(),
    getLayerState: vi.fn(),
    getBattery: vi.fn(),
    setLayerState: vi.fn(),
  },
  macroStore: {
    reset: vi.fn(),
    refreshOverview: vi.fn(),
  },
  terminalStore: {
    addEntry: vi.fn(),
  },
}));

vi.mock("@/services/HidService", () => ({
  hidService: mocks.hidService,
}));

vi.mock("@/stores/macroStore", () => ({
  useMacroStore: () => mocks.macroStore,
}));

vi.mock("@/stores/terminalStore", () => ({
  useTerminalStore: () => mocks.terminalStore,
}));

function receiverInfo(): DeviceInfo {
  return {
    vendorId: 0x413d,
    productId: 0x2107,
    chipFamily: "CH592F",
    versionMajor: 1,
    versionMinor: 0,
    versionPatch: 0,
    maxLayers: 4,
    maxKeys: 8,
    macroSlots: 8,
    keyboardType: KeyboardType.FIVE_KEYS,
    actualKeyCount: 0,
    fnKeyCount: 0,
    protocol: DeviceProtocol.CH592,
    protocolLabel: "CH592F HID",
    capabilities: createDeviceCapabilities({
      receiverRole: true,
      radio2g4: true,
    }),
  };
}

function status(currentLayer = 0): DeviceStatus {
  return {
    workMode: 2,
    connectionState: 1,
    currentLayer,
    batteryLevel: 90,
    isCharging: false,
  };
}

function prepareRemoteStore() {
  const store = useDeviceStore();
  const keymap = createEmptyKeymap();
  keymap.numLayers = 4;
  store.deviceInfo = receiverInfo();
  store.deviceStatus = status();
  store.remoteTargetReady = true;
  store.remoteTargetLoaded = true;
  store.keymapLoaded = true;
  store.keymap = keymap;
  store.keymapOriginal = JSON.parse(JSON.stringify(keymap));
  store.currentEditLayer = 0;
  return store;
}

describe("device store remote layer polling", () => {
  beforeEach(() => {
    vi.useFakeTimers();
    setActivePinia(createPinia());
    vi.clearAllMocks();
    mocks.hidService.getPairStatus.mockResolvedValue({
      state: "connected",
      linkConfirmed: true,
    });
    mocks.hidService.getSysStatus.mockResolvedValue(status());
    mocks.hidService.getReceiverBootLog.mockResolvedValue(null);
    mocks.hidService.getLayerState.mockResolvedValue({
      currentLayer: 1,
      numLayers: 4,
      defaultLayer: 0,
    });
    mocks.hidService.getBattery.mockResolvedValue({
      level: 90,
      isCharging: false,
      voltage: 4.0,
    });
  });

  afterEach(() => {
    vi.useRealTimers();
  });

  it("follows a physical layer switch without marking the keymap dirty", async () => {
    const store = prepareRemoteStore();

    store.startStatusPolling();
    await vi.advanceTimersByTimeAsync(2000);
    store.stopStatusPolling();

    expect(store.deviceStatus?.currentLayer).toBe(1);
    expect(store.keymap.currentLayer).toBe(1);
    expect(store.keymapOriginal.currentLayer).toBe(1);
    expect(store.currentEditLayer).toBe(1);
    expect(store.hasChanges).toBe(false);
  });

  it("keeps the edited layer selected when a physical switch arrives", async () => {
    const store = prepareRemoteStore();
    store.setKeyAction(0, {
      type: ActionType.KEYBOARD,
      modifier: 0,
      param1: 0x04,
      param2: 0,
    });
    expect(store.hasChanges).toBe(true);

    mocks.hidService.getLayerState.mockResolvedValue({
      currentLayer: 2,
      numLayers: 4,
      defaultLayer: 0,
    });
    store.startStatusPolling();
    await vi.advanceTimersByTimeAsync(2000);
    store.stopStatusPolling();

    expect(store.deviceStatus?.currentLayer).toBe(2);
    expect(store.keymap.currentLayer).toBe(2);
    expect(store.keymapOriginal.currentLayer).toBe(2);
    expect(store.currentEditLayer).toBe(0);
    expect(store.hasChanges).toBe(true);
  });

  it("keeps a manually selected edit layer while polling the current device layer", async () => {
    const store = prepareRemoteStore();
    store.deviceStatus = status(1);
    store.keymap.currentLayer = 1;
    store.keymapOriginal.currentLayer = 1;
    store.setEditLayer(0);

    mocks.hidService.getLayerState.mockResolvedValue({
      currentLayer: 1,
      numLayers: 4,
      defaultLayer: 0,
    });
    store.startStatusPolling();
    await vi.advanceTimersByTimeAsync(2000);
    store.stopStatusPolling();

    expect(store.deviceStatus?.currentLayer).toBe(1);
    expect(store.keymap.currentLayer).toBe(1);
    expect(store.keymapOriginal.currentLayer).toBe(1);
    expect(store.currentEditLayer).toBe(0);
    expect(store.hasChanges).toBe(false);
  });
});

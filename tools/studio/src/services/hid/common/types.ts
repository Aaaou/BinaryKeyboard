import type {
  DeviceInfo,
  DeviceStatus,
  KeymapConfig,
  RgbConfig,
  FnKeyConfig,
  LogConfig,
  OsModeConfig,
  DeviceProtocol,
  MacroOverview,
  MacroHeader,
  MacroData,
} from '@/types/protocol';
import type { DeviceUiProvider } from '@/types/deviceUi';
import type { TerminalEntryDraft } from './codecTypes';

export interface BatteryInfo {
  level: number;
  voltage: number;
  isCharging: boolean;
  adcRaw?: number;
  chargePinRaw?: number;
}

export interface LayerState {
  currentLayer: number;
  numLayers: number;
  defaultLayer: number;
}

export interface HidDeviceEvent {
  protocol: DeviceProtocol;
  frame: Uint8Array;
  entry: TerminalEntryDraft;
}

export type HidDeviceEventHandler = (event: HidDeviceEvent) => void;

export interface HidOptionalOperations {
  getRemoteDeviceInfo?: () => Promise<DeviceInfo>;
  getLayerState?: () => Promise<LayerState>;
  setLayerState?: (layer: number) => Promise<LayerState>;
  getRgbConfig?: () => Promise<RgbConfig>;
  setRgbConfig?: (config: RgbConfig) => Promise<void>;
  getFnKeyConfig?: () => Promise<FnKeyConfig>;
  setFnKeyConfig?: (config: FnKeyConfig) => Promise<void>;
  getOsMode?: () => Promise<OsModeConfig>;
  setOsMode?: (config: OsModeConfig) => Promise<void>;
  saveConfig?: () => Promise<void>;
  loadConfig?: () => Promise<void>;
  resetConfig?: () => Promise<void>;
  getBattery?: () => Promise<BatteryInfo>;
  getLogConfig?: () => Promise<LogConfig>;
  /** Receiver-only retained boot diagnostics. */
  getReceiverBootLog?: () => Promise<ReceiverBootLogEntry | null>;
  setLogConfig?: (config: LogConfig) => Promise<void>;
  getMacroOverview?: () => Promise<MacroOverview>;
  getMacroInfo?: (slot: number) => Promise<MacroHeader>;
  getMacroData?: (slot: number) => Promise<MacroData>;
  setMacroData?: (slot: number, macro: MacroData) => Promise<void>;
  deleteMacro?: (slot: number) => Promise<void>;
  getRadioCapabilities?: () => Promise<RadioCapabilities>;
  getPairStatus?: () => Promise<PairStatus>;
  startPairing?: () => Promise<void>;
  cancelPairing?: () => Promise<void>;
  clearPairing?: (force?: boolean) => Promise<void>;
  getPollRate?: () => Promise<number>;
  setPollRate?: (rate: number) => Promise<void>;
}

export interface ReceiverBootLogEntry {
  event: number;
  stage: number;
  result: number;
}

export interface RadioCapabilities {
  role: 'keyboard' | 'receiver';
  enabled: boolean;
  pollRates: number[];
}

export type PairState = 'unbound' | 'pairing' | 'bound' | 'connected' | 'inconsistent' | 'unsupported';

export interface PairStatus {
  state: PairState;
  session: number;
  deviceId: number;
  role?: 'keyboard' | 'receiver';
  peerDeviceId?: number;
  localId?: string;
  peerId?: string;
  fingerprint?: string;
  generation?: number;
  lastValidAgeMs?: number | null;
  protocolVersion?: number;
  hasPeer?: boolean;
  linkConfirmed?: boolean;
  pairingActive?: boolean;
  txEnqueued?: number;
  txBusy?: number;
  txFinished?: number;
  lastTxAgeMs?: number | null;
  txDescriptorsBusy?: number;
  lastLinkTimeoutAgeMs?: number | null;
  lastReleaseQueuedAgeMs?: number | null;
  lastReleaseSentAgeMs?: number | null;
  releaseBusyCount?: number;
  managementFlags?: number;
  managementTransaction?: number;
  managementCommand?: number;
  managementTxFragment?: number;
  managementTxFragments?: number;
  managementRxFragment?: number;
  managementRxFragments?: number;
  managementRxCount?: number;
  managementExecCount?: number;
  managementResponseTxCount?: number;
  rfValidFrameCount?: number;
  keyboardRfReportCount?: number;
  keyboardUsbSubmitCount?: number;
  keyboardUsbBusyCount?: number;
  rxQuarantined?: boolean;
}

export const OPTIONAL_OPERATION_LABELS: Record<keyof HidOptionalOperations, string> = {
  getRemoteDeviceInfo: '远端键盘能力读取',
  getLayerState: '当前层读取',
  setLayerState: '当前层切换',
  getRgbConfig: 'RGB 配置读取',
  setRgbConfig: 'RGB 配置写入',
  getFnKeyConfig: 'FN 键配置读取',
  setFnKeyConfig: 'FN 键配置写入',
  getOsMode: '系统模式读取',
  setOsMode: '系统模式写入',
  saveConfig: '配置保存',
  loadConfig: '配置加载',
  resetConfig: '恢复出厂设置',
  getBattery: '电池状态读取',
  getLogConfig: '日志配置读取',
  getReceiverBootLog: '读取接收器启动日志',
  setLogConfig: '日志配置写入',
  getMacroOverview: '宏概览读取',
  getMacroInfo: '宏信息读取',
  getMacroData: '宏数据读取',
  setMacroData: '宏数据写入',
  deleteMacro: '宏删除',
  getRadioCapabilities: '2.4G 能力读取',
  getPairStatus: '配码状态读取',
  startPairing: '开始配码',
  cancelPairing: '取消配码',
  clearPairing: '清除配码',
  getPollRate: '轮询率读取',
  setPollRate: '轮询率设置',
};

export interface HidAdapter {
  readonly protocol: DeviceProtocol;
  readonly filters: HIDDeviceFilter[];
  readonly optional: HidOptionalOperations;

  matches(device: HIDDevice): boolean;
  connect(device: HIDDevice): Promise<boolean>;
  disconnect(): Promise<void>;
  getDevice(): HIDDevice | null;
  isConnected(): boolean;
  onDeviceEvent(handler: HidDeviceEventHandler): () => void;

  getSysInfo(): Promise<DeviceInfo>;
  getSysStatus(): Promise<DeviceStatus>;
  getFullKeymap(): Promise<KeymapConfig>;
  setFullKeymap(config: KeymapConfig): Promise<void>;

  /** 发送原始 HID 帧并等待响应 (IAP 等底层操作使用) */
  sendRawFrame(frame: Uint8Array, timeout?: number): Promise<DataView>;
}

export interface HidDevicePlugin extends DeviceUiProvider {
  readonly id: string;
  readonly displayName: string;
  createAdapter(): HidAdapter;
}

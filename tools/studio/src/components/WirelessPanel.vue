<template>
  <div class="panel wireless-panel">
    <h3 class="panel-title"><i class="pi pi-wifi"></i>2.4G 无线与接收器</h3>
    <p class="hint">{{ modeHint }}</p>
    <div class="status-row"><span>配码状态</span><strong>{{ pairLabel }}</strong></div>
    <div class="status-row"><span>设备角色</span><strong>{{ roleLabel }}</strong></div>
    <div v-if="caps.enabled && status.state !== 'unsupported'" class="identity-block">
      <div><span>本机 ID</span><code>{{ status.localId || '未知' }}</code></div>
      <div><span>对端 ID</span><code>{{ status.peerId || (status.state === 'pairing' ? '配码中，尚未提交' : '未绑定') }}</code></div>
      <div><span>配对指纹</span><code>{{ status.fingerprint || '配码完成后生成' }}</code></div>
      <div><span>绑定代次/会话</span><code>{{ status.generation ?? status.session ?? '-' }}</code></div>
      <div><span>最近有效帧</span><code>{{ lastFrameLabel }}</code></div>
      <div><span>链路确认</span><code>{{ status.linkConfirmed ? '已收到有效帧' : '未确认' }}</code></div>
      <div><span>诊断协议</span><code>{{ status.protocolVersion ? `v${status.protocolVersion}` : '旧固件' }}</code></div>
      <template v-if="caps.role === 'receiver' && status.lastLinkTimeoutAgeMs !== undefined">
        <div><span>最近 RF 超时</span><code>{{ status.lastLinkTimeoutAgeMs == null ? '尚未发生' : `${status.lastLinkTimeoutAgeMs} ms 前` }}</code></div>
        <div><span>释放排队</span><code>{{ status.lastReleaseQueuedAgeMs == null ? '尚未发生' : `${status.lastReleaseQueuedAgeMs} ms 前` }}</code></div>
        <div><span>释放提交</span><code>{{ status.lastReleaseSentAgeMs == null ? '尚未发生' : `${status.lastReleaseSentAgeMs} ms 前` }}</code></div>
        <div><span>USB 忙次数</span><code>{{ status.releaseBusyCount ?? 0 }}</code></div>
      </template>
      <template v-if="caps.role === 'keyboard' && status.txEnqueued !== undefined">
        <div><span>RF TX 入队/完成</span><code>{{ status.txEnqueued }} / {{ status.txFinished }}</code></div>
        <div><span>RF TX DMA 忙</span><code>{{ status.txBusy }} 次</code></div>
        <div><span>TX 描述符占用</span><code>{{ status.txDescriptorsBusy }} / 16</code></div>
        <div><span>最近 TX</span><code>{{ status.lastTxAgeMs == null ? '尚未发送' : `${status.lastTxAgeMs} ms 前` }}</code></div>
      </template>
    </div>
    <div class="actions">
      <button type="button" :disabled="controlsDisabled" @click="start">开始配码</button>
      <button type="button" :disabled="controlsDisabled" @click="cancel">取消配码</button>
      <button type="button" class="danger" :disabled="controlsDisabled" @click="clear">清除绑定</button>
    </div>
    <div v-if="caps.role === 'receiver'" class="poll-row">
      <label for="radio-poll-rate">USB 轮询率</label>
      <select id="radio-poll-rate" v-model.number="pollRate" :disabled="busy" @change="savePollRate">
        <option v-for="rate in pollRates" :key="rate" :value="rate">{{ rate }} Hz</option>
      </select>
    </div>
    <small>配码命令只作用于当前通过 USB 连接的设备。接收器和键盘需要分别进入配码窗口：先连接接收器开始配码，再连接键盘开始配码；两端都显示“已绑定”后才算完成。更换设备时清除对应一端即可重新配码。</small>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, onUnmounted, ref } from 'vue';
import { hidService } from '@/services/HidService';
import { showToast } from '@/services/toastService';
import { useDeviceStore } from '@/stores/deviceStore';
import type { PairStatus, RadioCapabilities } from '@/services/hid/common/types';

const busy = ref(false);
const deviceStore = useDeviceStore();
const caps = ref<RadioCapabilities>({ role: 'keyboard', enabled: false, pollRates: [125, 250, 500, 1000] });
const status = ref<PairStatus>({ state: 'unsupported', session: 0, deviceId: 0 });
const pollRate = ref(1000);
const pollRates = computed(() => caps.value.pollRates.length ? caps.value.pollRates : [125, 250, 500, 1000]);
const pairLabel = computed(() => ({ unbound: '未绑定', pairing: '配码中', bound: '已绑定', connected: '已连接', inconsistent: '两端绑定不一致', unsupported: '未启用' }[status.value.state]));
const roleLabel = computed(() => caps.value.role === 'receiver' ? '接收器' : '键盘');
const lastFrameLabel = computed(() => status.value.lastValidAgeMs == null ? '尚未收到' : `${status.value.lastValidAgeMs} ms 前`);
const radioImageActive = computed(() => caps.value.role === 'receiver' || deviceStore.deviceStatus?.workMode === 2);
const controlsDisabled = computed(() => busy.value || !caps.value.enabled || !radioImageActive.value);
const modeHint = computed(() => {
  if (caps.value.role === 'receiver' || radioImageActive.value) return '当前 2.4G 射频实例可执行配码和绑定维护。';
  if (deviceStore.capabilities.trimode) return '当前是 USB/BLE 子镜像；切换到 2.4G 模式并等待设备重新连接后可配码。';
  return '当前固件未启用 2.4G 射频实例。';
});

async function refresh() {
  try {
    caps.value = await hidService.getRadioCapabilities();
    status.value = await hidService.getPairStatus();
    if (caps.value.role === 'receiver') {
      pollRate.value = await hidService.getPollRate();
    }
  } catch (error) {
    showToast('warn', '2.4G 不可用', error instanceof Error ? error.message : '当前固件未启用 RF 后端');
  }
}
async function run(action: () => Promise<void>, message: string) {
  busy.value = true;
  try { await action(); await refresh(); showToast('success', '操作完成', message); }
  catch (error) { showToast('error', '操作失败', error instanceof Error ? error.message : '设备拒绝了操作'); }
  finally { busy.value = false; }
}
async function start() {
  if (!radioImageActive.value) {
    showToast('warn', '请先切换模式', '三模键盘需要先切换到 2.4G 模式，重新连接后才能开始配码');
    return;
  }
  busy.value = true;
  try {
    // Pairing starts an RF role asynchronously.  Acknowledging the request is
    // deliberately distinct from completing a pairing session, so a host
    // startup that takes time cannot be reported as a UI command timeout.
    await hidService.startPairing();
    status.value = { ...status.value, state: 'pairing' };
    const target = caps.value.role === 'receiver' ? '接收器' : '键盘';
    showToast('success', '配码请求已受理', `${target}正在进入 60 秒配码窗口`);
  } catch (error) {
    showToast('error', '操作失败', error instanceof Error ? error.message : '设备拒绝了操作');
  } finally {
    busy.value = false;
  }
}
const cancel = () => run(() => hidService.cancelPairing(), '已取消配码');
const clear = () => run(() => hidService.clearPairing(false), '已请求双端清除；远端离线时请使用重新配码');
const savePollRate = () => run(() => hidService.setPollRate(pollRate.value), `已设置为 ${pollRate.value} Hz`);
let refreshTimer: number | undefined;
onMounted(() => {
  void refresh();
  refreshTimer = window.setInterval(() => { if (!busy.value) void refresh(); }, 2000);
});
onUnmounted(() => { if (refreshTimer !== undefined) window.clearInterval(refreshTimer); });
</script>

<style scoped>
.wireless-panel { display: flex; flex-direction: column; gap: 0.8rem; }
.hint, small { color: var(--c-text-secondary); font-size: 0.78rem; line-height: 1.5; }
.status-row, .poll-row { display: flex; justify-content: space-between; align-items: center; gap: 0.7rem; }
.identity-block { display: grid; gap: 0.35rem; border-top: 1px solid var(--c-border); padding-top: 0.65rem; }
.identity-block > div { display: flex; justify-content: space-between; gap: 0.7rem; font-size: 0.78rem; }
.identity-block code { color: var(--c-text-primary); font-family: ui-monospace, SFMono-Regular, Consolas, monospace; word-break: break-all; text-align: right; }
.actions { display: flex; flex-wrap: wrap; gap: 0.5rem; }
button, select { border: 1px solid var(--c-border); border-radius: 6px; padding: 0.45rem 0.7rem; background: var(--c-bg-tertiary); color: var(--c-text-primary); }
button { cursor: pointer; } button.danger { color: var(--c-danger); } button:disabled { opacity: 0.5; cursor: wait; }
</style>

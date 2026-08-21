<template>
  <div class="panel wireless-panel">
    <h3 class="panel-title"><i class="pi pi-wifi"></i>2.4G 无线与接收器</h3>
    <p class="hint">仅 RF-enabled 固件显示。当前普通 CH592F 键盘未集成 2.4G 后端，不会接受配码命令。</p>
    <div class="status-row"><span>配码状态</span><strong>{{ pairLabel }}</strong></div>
    <div class="status-row"><span>设备角色</span><strong>{{ roleLabel }}</strong></div>
    <div class="actions">
      <button type="button" :disabled="busy" @click="start">开始配码</button>
      <button type="button" :disabled="busy" @click="cancel">取消配码</button>
      <button type="button" class="danger" :disabled="busy" @click="clear">清除绑定</button>
    </div>
    <div class="poll-row">
      <label for="radio-poll-rate">USB 轮询率</label>
      <select id="radio-poll-rate" v-model.number="pollRate" :disabled="busy" @change="savePollRate">
        <option v-for="rate in pollRates" :key="rate" :value="rate">{{ rate }} Hz</option>
      </select>
    </div>
    <small>首次配对：先让接收器进入等待配码，再点击键盘“开始配码”；成功后双方保存同一绑定摘要。更换设备请使用“重新配码”，不要只清除一端。</small>
  </div>
</template>

<script setup lang="ts">
import { computed, onMounted, ref } from 'vue';
import { hidService } from '@/services/HidService';
import { showToast } from '@/services/toastService';
import type { PairStatus, RadioCapabilities } from '@/services/hid/common/types';

const busy = ref(false);
const caps = ref<RadioCapabilities>({ role: 'keyboard', enabled: false, pollRates: [125, 250, 500, 1000] });
const status = ref<PairStatus>({ state: 'unsupported', session: 0, deviceId: 0 });
const pollRate = ref(1000);
const pollRates = computed(() => caps.value.pollRates.length ? caps.value.pollRates : [125, 250, 500, 1000]);
const pairLabel = computed(() => ({ unbound: '未绑定', pairing: '配码中', bound: '已绑定', connected: '已连接', inconsistent: '两端绑定不一致', unsupported: '未启用' }[status.value.state]));
const roleLabel = computed(() => caps.value.role === 'receiver' ? '接收器' : '键盘');

async function refresh() {
  try {
    caps.value = await hidService.getRadioCapabilities();
    status.value = await hidService.getPairStatus();
    pollRate.value = await hidService.getPollRate();
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
const start = () => run(() => hidService.startPairing(), '已进入 60 秒配码窗口');
const cancel = () => run(() => hidService.cancelPairing(), '已取消配码');
const clear = () => run(() => hidService.clearPairing(false), '已请求双端清除；远端离线时请使用重新配码');
const savePollRate = () => run(() => hidService.setPollRate(pollRate.value), `已设置为 ${pollRate.value} Hz`);
onMounted(() => void refresh());
</script>

<style scoped>
.wireless-panel { display: flex; flex-direction: column; gap: 0.8rem; }
.hint, small { color: var(--c-text-secondary); font-size: 0.78rem; line-height: 1.5; }
.status-row, .poll-row { display: flex; justify-content: space-between; align-items: center; gap: 0.7rem; }
.actions { display: flex; flex-wrap: wrap; gap: 0.5rem; }
button, select { border: 1px solid var(--c-border); border-radius: 6px; padding: 0.45rem 0.7rem; background: var(--c-bg-tertiary); color: var(--c-text-primary); }
button { cursor: pointer; } button.danger { color: var(--c-danger); } button:disabled { opacity: 0.5; cursor: wait; }
</style>

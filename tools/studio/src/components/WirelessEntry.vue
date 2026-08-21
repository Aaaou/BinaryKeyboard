<template>
  <StudioSidebarEntry
    title="2.4G 无线与接收器"
    :meta="meta"
    icon="pi pi-wifi"
    aria-label="打开 2.4G 无线与接收器设置"
    @open="emit('open')"
  />
</template>

<script setup lang="ts">
import { computed } from 'vue';
import { useDeviceStore } from '@/stores/deviceStore';
import StudioSidebarEntry from '@/components/StudioSidebarEntry.vue';

const emit = defineEmits<{ (e: 'open'): void }>();
const deviceStore = useDeviceStore();
const meta = computed(() => deviceStore.deviceInfo?.capabilities.receiverRole ? '接收器已连接' : '键盘 2.4G 设置');
</script>

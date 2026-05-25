<template>
  <BatteryChart :readings="readings" field="ocv_est" label="OCV_EST" unit="V" color="#fb7185" :decimals="4" />
</template>

<script setup>
import BatteryChart from './BatteryChart.vue'
defineProps({ readings: { type: Array, default: () => [] } })
</script>

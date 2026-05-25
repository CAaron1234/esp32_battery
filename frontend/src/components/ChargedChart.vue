<template>
  <BatteryChart :readings="readings" field="charged_ah" label="CHARGED" unit="Ah" color="#a3e635" :decimals="4" />
</template>

<script setup>
import BatteryChart from './BatteryChart.vue'
defineProps({ readings: { type: Array, default: () => [] } })
</script>

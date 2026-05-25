<template>
  <BatteryChart :readings="readings" field="i_dps" label="I_DPS" unit="A" color="#38bdf8" :decimals="4" />
</template>

<script setup>
import BatteryChart from './BatteryChart.vue'
defineProps({ readings: { type: Array, default: () => [] } })
</script>

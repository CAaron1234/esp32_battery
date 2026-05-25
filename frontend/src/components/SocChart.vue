<template>
  <BatteryChart :readings="readings" field="soc" label="SOC" unit="%" color="#22d3ee" :decimals="1" />
</template>

<script setup>
import BatteryChart from './BatteryChart.vue'
defineProps({ readings: { type: Array, default: () => [] } })
</script>

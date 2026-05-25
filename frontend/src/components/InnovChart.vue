<template>
  <BatteryChart :readings="readings" field="innov" label="INNOV" unit="V" color="#fb923c" :decimals="4" />
</template>

<script setup>
import BatteryChart from './BatteryChart.vue'
defineProps({ readings: { type: Array, default: () => [] } })
</script>

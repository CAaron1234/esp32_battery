<template>
  <BatteryChart :readings="readings" field="v_dps" label="V_DPS" unit="V" color="#fbbf24" :decimals="4" />
</template>

<script setup>
import BatteryChart from './BatteryChart.vue'
defineProps({ readings: { type: Array, default: () => [] } })
</script>

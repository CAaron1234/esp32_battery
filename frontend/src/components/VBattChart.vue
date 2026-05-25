<template>
  <BatteryChart :readings="readings" field="v_batt" label="V_BATT" unit="V" color="#34d399" :decimals="4" />
</template>

<script setup>
import BatteryChart from './BatteryChart.vue'
defineProps({ readings: { type: Array, default: () => [] } })
</script>

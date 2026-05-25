<template>
  <BatteryChart :readings="readings" field="v_rc" label="V_RC" unit="V" color="#a78bfa" :decimals="5" />
</template>

<script setup>
import BatteryChart from './BatteryChart.vue'
defineProps({ readings: { type: Array, default: () => [] } })
</script>

<template>
  <div class="relative w-full h-48 md:h-56">
    <Line :data="chartData" :options="chartOptions" />
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { Line } from 'vue-chartjs'
import {
  Chart as ChartJS,
  LineElement,
  PointElement,
  LinearScale,
  CategoryScale,
  Tooltip,
  Filler,
} from 'chart.js'

ChartJS.register(LineElement, PointElement, LinearScale, CategoryScale, Tooltip, Filler)

const props = defineProps({
  readings: { type: Array,  default: () => [] },
  field:    { type: String, required: true },
  label:    { type: String, required: true },
  unit:     { type: String, default: '' },
  color:    { type: String, default: '#34d399' },
  decimals: { type: Number, default: 4 },
})

const chartData = computed(() => ({
  labels: props.readings.map(r => {
    const d = new Date(r.timestamp)
    return isNaN(d) ? r.timestamp : d.toLocaleTimeString('en-MY')
  }),
  datasets: [{
    label: `${props.label}`,
    data:  props.readings.map(r => r[props.field]),
    borderColor:          props.color,
    backgroundColor:      props.color + '18',
    borderWidth:          2,
    pointRadius:          props.readings.length > 60 ? 0 : 3,
    pointHoverRadius:     5,
    pointBackgroundColor: props.color,
    tension:              0.4,
    fill:                 true,
  }],
}))

const chartOptions = computed(() => ({
  responsive:          true,
  maintainAspectRatio: false,
  animation: { duration: 200 },
  scales: {
    x: {
      ticks: {
        color: '#52525b',
        font:  { family: "'DM Mono', monospace", size: 10 },
        maxTicksLimit: 6,
        maxRotation:   0,
      },
      grid:   { color: 'rgba(255,255,255,0.04)' },
      border: { color: 'rgba(255,255,255,0.08)' },
    },
    y: {
      ticks: {
        color: '#52525b',
        font:  { family: "'DM Mono', monospace", size: 10 },
        callback: (val) => `${val}${props.unit}`,
      },
      grid:   { color: 'rgba(255,255,255,0.04)' },
      border: { color: 'rgba(255,255,255,0.08)' },
    },
  },
  plugins: {
    tooltip: {
      backgroundColor: '#18181b',
      borderColor:     props.color,
      borderWidth:     1,
      titleColor:      '#a1a1aa',
      bodyColor:       '#ffffff',
      titleFont: { family: "'DM Mono', monospace", size: 10 },
      bodyFont:  { family: "'DM Mono', monospace", size: 13 },
      callbacks: {
        label: (ctx) => ` ${ctx.parsed.y.toFixed(props.decimals)} ${props.unit}`,
      },
    },
    legend: { display: false },
  },
}))
</script>

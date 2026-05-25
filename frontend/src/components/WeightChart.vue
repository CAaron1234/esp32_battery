<template>
  <div class="relative w-full h-72 md:h-96">
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
  readings: {
    type: Array,
    default: () => [],
  },
})

// ── Chart Data ───────────────────────────────────────────
const chartData = computed(() => ({
  labels: props.readings.map(r => {
    // Show only time portion: HH:MM:SS
    const d = new Date(r.timestamp)
    return isNaN(d) ? r.timestamp : d.toLocaleTimeString('en-MY')
  }),

  datasets: [
    {
      label: 'Weight (g)',
      data: props.readings.map(r => r.weight),
      borderColor:     '#34d399',       // emerald-400
      backgroundColor: 'rgba(52,211,153,0.08)',
      borderWidth: 2,
      pointRadius: props.readings.length > 60 ? 0 : 3,  // hide dots when dense
      pointHoverRadius: 5,
      pointBackgroundColor: '#34d399',
      tension: 0.4,
      fill: true,
    },
  ],
}))

// ── Chart Options ────────────────────────────────────────
const chartOptions = {
  responsive:          true,
  maintainAspectRatio: false,
  animation: {
    duration: 200,   // fast update animation
  },
  scales: {
    x: {
      ticks: {
        color:    '#52525b',       // zinc-600
        font:     { family: "'DM Mono', monospace", size: 10 },
        maxTicksLimit: 8,
        maxRotation: 0,
      },
      grid: {
        color: 'rgba(255,255,255,0.04)',
      },
      border: {
        color: 'rgba(255,255,255,0.08)',
      },
    },
    y: {
      ticks: {
        color: '#52525b',
        font:  { family: "'DM Mono', monospace", size: 10 },
        callback: (val) => `${val}g`,
      },
      grid: {
        color: 'rgba(255,255,255,0.04)',
      },
      border: {
        color: 'rgba(255,255,255,0.08)',
      },
    },
  },
  plugins: {
    tooltip: {
      backgroundColor: '#18181b',
      borderColor:     '#34d399',
      borderWidth:     1,
      titleColor:      '#a1a1aa',
      bodyColor:       '#ffffff',
      titleFont:       { family: "'DM Mono', monospace", size: 10 },
      bodyFont:        { family: "'DM Mono', monospace", size: 13 },
      callbacks: {
        label: (ctx) => ` ${ctx.parsed.y.toFixed(2)} g`,
      },
    },
    legend: { display: false },
  },
}
</script>
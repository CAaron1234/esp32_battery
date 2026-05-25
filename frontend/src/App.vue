<template>
  <div class="min-h-screen bg-[#0a0a0a] px-6 py-10 md:px-16">

    <!-- Header -->
    <header class="mb-10">
      <div class="flex items-center gap-3 mb-1">
        <span :class="['w-2.5 h-2.5 rounded-full', isConnected ? 'bg-emerald-400 animate-pulse-dot' : 'bg-red-500']" />
        <span class="text-xs tracking-widest uppercase font-body"
              :class="isConnected ? 'text-emerald-400' : 'text-red-400'">
          {{ isConnected ? 'Live' : 'Disconnected' }}
        </span>
      </div>
      <h1 class="font-display text-3xl md:text-8xl tracking-wider text-white leading-none">
        BATTERY MONITOR
      </h1>
      <p class="text-zinc-500 text-xs tracking-widest uppercase mt-2 font-body">
        ESP32 · DPS5015 · EKF SOC Estimator
      </p>
    </header>

    <!-- Primary Stat Cards -->
    <div class="grid grid-cols-2 md:grid-cols-4 gap-3 mb-3">

      <!-- SOC with progress bar -->
      <div class="bg-zinc-900 border border-zinc-800 rounded-xl p-4 col-span-2 md:col-span-1">
        <p class="text-zinc-500 text-xs tracking-widest uppercase mb-1">SOC</p>
        <p class="font-display text-4xl text-white tracking-wide">
          {{ latest ? latest.soc.toFixed(1) : '—' }}<span class="text-lg text-zinc-400">%</span>
        </p>
        <div class="mt-2 h-1.5 bg-zinc-800 rounded-full overflow-hidden">
          <div class="h-full bg-cyan-400 rounded-full transition-all duration-500"
               :style="{ width: latest ? latest.soc + '%' : '0%' }" />
        </div>
      </div>

      <div class="bg-zinc-900 border border-zinc-800 rounded-xl p-4">
        <p class="text-zinc-500 text-xs tracking-widest uppercase mb-1">V_BATT</p>
        <p class="font-display text-4xl text-white tracking-wide">
          {{ latest ? latest.v_batt.toFixed(3) : '—' }}<span class="text-lg text-zinc-400">V</span>
        </p>
      </div>

      <div class="bg-zinc-900 border border-zinc-800 rounded-xl p-4">
        <p class="text-zinc-500 text-xs tracking-widest uppercase mb-1">I_DPS</p>
        <p class="font-display text-4xl text-white tracking-wide">
          {{ latest ? latest.i_dps.toFixed(3) : '—' }}<span class="text-lg text-zinc-400">A</span>
        </p>
      </div>

      <div class="bg-zinc-900 border border-zinc-800 rounded-xl p-4">
        <p class="text-zinc-500 text-xs tracking-widest uppercase mb-1">Charger</p>
        <p class="font-display text-4xl tracking-wide"
           :class="latest?.charger === 'CC' ? 'text-sky-400' : latest?.charger === 'CV' ? 'text-amber-400' : 'text-zinc-500'">
          {{ latest?.charger || '—' }}
        </p>
      </div>

    </div>

    <!-- Secondary Stat Cards -->
    <div class="grid grid-cols-2 md:grid-cols-4 gap-3 mb-8">

      <div class="bg-zinc-900 border border-zinc-800 rounded-xl p-4">
        <p class="text-zinc-500 text-xs tracking-widest uppercase mb-1">V_DPS</p>
        <p class="font-display text-3xl text-white tracking-wide">
          {{ latest ? latest.v_dps.toFixed(3) : '—' }}<span class="text-base text-zinc-400">V</span>
        </p>
      </div>

      <div class="bg-zinc-900 border border-zinc-800 rounded-xl p-4">
        <p class="text-zinc-500 text-xs tracking-widest uppercase mb-1">OCV_EST</p>
        <p class="font-display text-3xl text-white tracking-wide">
          {{ latest ? latest.ocv_est.toFixed(3) : '—' }}<span class="text-base text-zinc-400">V</span>
        </p>
      </div>

      <div class="bg-zinc-900 border border-zinc-800 rounded-xl p-4">
        <p class="text-zinc-500 text-xs tracking-widest uppercase mb-1">Innov</p>
        <p class="font-display text-3xl tracking-wide"
           :class="latest && Math.abs(latest.innov) < 0.05 ? 'text-emerald-400' : 'text-orange-400'">
          {{ latest ? latest.innov.toFixed(4) : '—' }}<span class="text-base text-zinc-400">V</span>
        </p>
      </div>

      <div class="bg-zinc-900 border border-zinc-800 rounded-xl p-4">
        <p class="text-zinc-500 text-xs tracking-widest uppercase mb-1">Charged</p>
        <p class="font-display text-3xl text-white tracking-wide">
          {{ latest ? latest.charged_ah.toFixed(3) : '—' }}<span class="text-base text-zinc-400">Ah</span>
        </p>
      </div>

    </div>

    <!-- Chart Controls -->
    <div class="flex items-center justify-between mb-4">
      <h2 class="font-display text-2xl tracking-widest text-white">LIVE CHARTS</h2>
      <div class="flex gap-2">
        <button v-for="n in [30, 60, 100]" :key="n" @click="maxPoints = n"
          :class="['px-3 py-1 rounded-lg text-xs tracking-widest uppercase transition-all duration-200',
            maxPoints === n ? 'bg-emerald-400 text-black font-medium' : 'bg-zinc-800 text-zinc-400 hover:bg-zinc-700']">
          {{ n }}pts
        </button>
      </div>
    </div>

    <!-- Charts Grid -->
    <div class="grid grid-cols-1 md:grid-cols-2 gap-4">

      <!-- SOC -->
      <div class="bg-zinc-900 border border-zinc-800 rounded-2xl p-5">
        <div class="flex justify-between items-baseline mb-3">
          <p class="text-xs tracking-widest uppercase text-cyan-400 font-body">SOC</p>
          <p class="font-display text-xl text-white">
            {{ latest ? latest.soc.toFixed(1) : '—' }}<span class="text-sm text-zinc-400">%</span>
          </p>
        </div>
        <SocChart :readings="sliced" />
      </div>

      <!-- V_BATT -->
      <div class="bg-zinc-900 border border-zinc-800 rounded-2xl p-5">
        <div class="flex justify-between items-baseline mb-3">
          <p class="text-xs tracking-widest uppercase text-emerald-400 font-body">V_BATT</p>
          <p class="font-display text-xl text-white">
            {{ latest ? latest.v_batt.toFixed(4) : '—' }}<span class="text-sm text-zinc-400">V</span>
          </p>
        </div>
        <VBattChart :readings="sliced" />
      </div>

      <!-- I_DPS -->
      <div class="bg-zinc-900 border border-zinc-800 rounded-2xl p-5">
        <div class="flex justify-between items-baseline mb-3">
          <p class="text-xs tracking-widest uppercase text-sky-400 font-body">I_DPS</p>
          <p class="font-display text-xl text-white">
            {{ latest ? latest.i_dps.toFixed(4) : '—' }}<span class="text-sm text-zinc-400">A</span>
          </p>
        </div>
        <IDpsChart :readings="sliced" />
      </div>

      <!-- V_DPS -->
      <div class="bg-zinc-900 border border-zinc-800 rounded-2xl p-5">
        <div class="flex justify-between items-baseline mb-3">
          <p class="text-xs tracking-widest uppercase text-amber-400 font-body">V_DPS</p>
          <p class="font-display text-xl text-white">
            {{ latest ? latest.v_dps.toFixed(4) : '—' }}<span class="text-sm text-zinc-400">V</span>
          </p>
        </div>
        <VDpsChart :readings="sliced" />
      </div>

      <!-- OCV_EST -->
      <div class="bg-zinc-900 border border-zinc-800 rounded-2xl p-5">
        <div class="flex justify-between items-baseline mb-3">
          <p class="text-xs tracking-widest uppercase text-rose-400 font-body">OCV_EST</p>
          <p class="font-display text-xl text-white">
            {{ latest ? latest.ocv_est.toFixed(4) : '—' }}<span class="text-sm text-zinc-400">V</span>
          </p>
        </div>
        <OcvEstChart :readings="sliced" />
      </div>

      <!-- INNOV -->
      <div class="bg-zinc-900 border border-zinc-800 rounded-2xl p-5">
        <div class="flex justify-between items-baseline mb-3">
          <p class="text-xs tracking-widest uppercase text-orange-400 font-body">INNOV</p>
          <p class="font-display text-xl text-white">
            {{ latest ? latest.innov.toFixed(4) : '—' }}<span class="text-sm text-zinc-400">V</span>
          </p>
        </div>
        <InnovChart :readings="sliced" />
      </div>

      <!-- V_RC -->
      <div class="bg-zinc-900 border border-zinc-800 rounded-2xl p-5">
        <div class="flex justify-between items-baseline mb-3">
          <p class="text-xs tracking-widest uppercase text-violet-400 font-body">V_RC</p>
          <p class="font-display text-xl text-white">
            {{ latest ? latest.v_rc.toFixed(5) : '—' }}<span class="text-sm text-zinc-400">V</span>
          </p>
        </div>
        <VRcChart :readings="sliced" />
      </div>

      <!-- CHARGED -->
      <div class="bg-zinc-900 border border-zinc-800 rounded-2xl p-5">
        <div class="flex justify-between items-baseline mb-3">
          <p class="text-xs tracking-widest uppercase text-lime-400 font-body">CHARGED</p>
          <p class="font-display text-xl text-white">
            {{ latest ? latest.charged_ah.toFixed(4) : '—' }}<span class="text-sm text-zinc-400">Ah</span>
          </p>
        </div>
        <ChargedChart :readings="sliced" />
      </div>

    </div>

    <!-- Footer -->
    <p class="text-center text-zinc-600 text-xs tracking-widest uppercase mt-8 font-body">
      Last updated: {{ lastUpdated || '—' }}
    </p>

  </div>
</template>

<script setup>
import { ref, computed, onMounted, onUnmounted } from 'vue'
import SocChart     from './components/SocChart.vue'
import VBattChart   from './components/VBattChart.vue'
import VDpsChart    from './components/VDpsChart.vue'
import IDpsChart    from './components/IDpsChart.vue'
import OcvEstChart  from './components/OcvEstChart.vue'
import InnovChart   from './components/InnovChart.vue'
import VRcChart     from './components/VRcChart.vue'
import ChargedChart from './components/ChargedChart.vue'

// ── State ───────────────────────────────────────────────────
const readings    = ref([])
const isConnected = ref(false)
const maxPoints   = ref(60)
const lastUpdated = ref(null)

let ws = null

// ── Computed ─────────────────────────────────────────────────
const sliced = computed(() => readings.value.slice(-maxPoints.value))

const latest = computed(() => readings.value.length ? readings.value.at(-1) : null)

// ── WebSocket ─────────────────────────────────────────────────
function connect() {
  ws = new WebSocket('ws://localhost:3000')

  ws.onopen = () => {
    isConnected.value = true
    console.log('[WS] Connected')
  }

  ws.onmessage = (event) => {
    const msg = JSON.parse(event.data)

    if (msg.type === 'lee9_history') {
      readings.value = msg.data
    }

    if (msg.type === 'lee9_reading') {
      readings.value.push(msg.data)
      lastUpdated.value = msg.data.timestamp
    }
  }

  ws.onclose = () => {
    isConnected.value = false
    console.log('[WS] Disconnected. Retrying in 3s...')
    setTimeout(connect, 3000)
  }

  ws.onerror = (err) => {
    console.error('[WS] Error:', err)
    ws.close()
  }
}

onMounted(() => connect())
onUnmounted(() => ws?.close())
</script>

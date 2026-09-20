<template>
  <div class="screen">
    <!-- 视频区：原生模块直写屏幕。点按唤出/收起控制栏 -->
    <div class="videoarea" @click="toggleControls">
      <div class="errbox" v-if="errorText">
        <text class="errtext">{{ errorText }}</text>
      </div>
      <div class="hintbox" v-if="showHint">
        <text class="hinttext">点按画面唤出控制</text>
      </div>
    </div>

    <!-- 控制栏：默认隐藏，点按视频唤出；唤出时视频区收缩到其上方，互不遮挡 -->
    <div class="controls" v-if="controlsVisible">
      <div class="row1">
        <text class="time">{{ fmtTime(positionMs) }}</text>
        <slider class="seekbar" :min="0" :max="seekMax" :step="1" v-model="seekVal"
          active-color="#4fd6c3" background-color="#263340"
          @moving="onSeekMoving" @change="onSeekChange"></slider>
        <text class="time right">{{ durText }}</text>
      </div>
      <div class="row2">
        <div class="btn press" @click="togglePlay"><text class="btntext">{{ playing ? '暂停' : '播放' }}</text></div>
        <div class="btn press" @click="seekBy(-30)"><text class="btntext">-30s</text></div>
        <div class="btn press" @click="seekBy(30)"><text class="btntext">+30s</text></div>
        <div class="btn accent press" @click="cycleRate"><text class="btntextacc">{{ rateLabel }}</text></div>
        <div class="volbox">
          <text class="volicon">音</text>
          <slider class="volbar" :min="0" :max="100" :step="5" v-model="volume"
            active-color="#f5b85c" background-color="#263340" @change="onVolumeChange"></slider>
          <text class="voltext">{{ volume }}</text>
        </div>
        <text class="qtag" v-if="qualityLabel">{{ qualityLabel }}</text>
        <div class="btn back press" @click="goBack"><text class="btntext">返回</text></div>
      </div>
    </div>

    <app-toast></app-toast>
  </div>
</template>

<script>
import bridge from '../../utils/player-bridge.js'
import quality from '../../utils/quality.js'
import history from '../../utils/history.js'
import device from '../../utils/device.js'
import { fmtTime } from '../../utils/fmt.js'
import appToast from '../../components/app-toast.vue'

const FULL = { rectX: 0, rectY: 0, rectW: 800, rectH: 254 }   // 控制栏隐藏：全屏视频
const WITHBAR = { rectX: 0, rectY: 0, rectW: 800, rectH: 192 } // 控制栏唤出：视频收缩到上方

export default {
  components: { 'app-toast': appToast },
  data() {
    return {
      path: '',
      name: '',
      positionMs: 0,
      durationMs: 0,
      seekVal: 0,
      seekMax: 1000,
      scrubbing: false,
      playing: false,
      ended: false,
      rate: 1,
      volume: 70,
      screen: { w: 800, h: 254 },
      qualityLabel: '',
      errorText: '',
      controlsVisible: false,
      showHint: true
    }
  },
  computed: {
    rateLabel() {
      const r = Number(this.rate) || 1
      return (r === Math.floor(r) ? r.toFixed(0) : String(r)) + 'x'
    },
    durText() {
      return this.durationMs > 0 ? fmtTime(this.durationMs) : '--:--'
    }
  },
  methods: {
    fmtTime,
    toast(text, ms) {
      $falcon.trigger('bpv-toast', { text, ms })
    },
    goBack() {
      this.$page.finish()
    },
    // 点按视频区：唤出/收起控制栏，并同步收缩视频区域（避免遮挡）
    toggleControls() {
      this.controlsVisible = !this.controlsVisible
      this.showHint = false
      bridge.setView(this.controlsVisible ? WITHBAR : FULL)
      if (this.controlsVisible) this.syncProgress()
    },
    async start(path, resumeMs) {
      const r = await bridge.play({
        path,
        screenW: this.screen.w,
        screenH: this.screen.h,
        rectX: FULL.rectX,
        rectY: FULL.rectY,
        rectW: FULL.rectW,
        rectH: FULL.rectH,
        maxW: this.screen.w,
        maxH: this.screen.h,
        rate: this.rate,
        volume: this.volume,
      })
      if (!r || !r.ok) {
        this.playing = false
        this.errorText = '无法播放\n' + ((r && r.error) || '未知错误')
        this.toast('无法播放：' + ((r && r.error) || '未知错误'), 3200)
        return
      }
      this.errorText = ''
      this.playing = true
      if (r.durationMs > 0) this.durationMs = r.durationMs
      const q = quality.decideQuality({
        videoW: r.videoW, videoH: r.videoH,
        screenW: this.screen.w, screenH: this.screen.h,
        name: this.name, mode: 'auto',
      })
      this.qualityLabel = q.label
      if (q.action === 'downscale') this.toast(q.label)
      if (resumeMs > 3000) {
        await bridge.seek(resumeMs)
        this.positionMs = resumeMs
        this.syncProgress()
      }
    },
    togglePlay() {
      if (this.ended) {
        this.ended = false
        bridge.seek(0)
        bridge.resume()
        this.playing = true
        return
      }
      if (this.playing) {
        bridge.pause()
        this.playing = false
      } else {
        bridge.resume()
        this.playing = true
      }
    },
    seekBy(sec) {
      const target = Math.max(0, this.positionMs + sec * 1000)
      bridge.seek(target)
      this.positionMs = target
      this.syncProgress()
    },
    onSeekMoving(val) {
      this.scrubbing = true
      if (this.durationMs > 0) this.positionMs = Math.round(val / this.seekMax * this.durationMs)
    },
    onSeekChange(val) {
      this.scrubbing = false
      if (this.durationMs > 0) {
        const target = Math.round(val / this.seekMax * this.durationMs)
        bridge.seek(target)
        this.positionMs = target
      }
    },
    async cycleRate() {
      const list = quality.RATES
      let i = list.indexOf(Number(this.rate) || 1)
      if (i === -1) i = 2
      this.rate = list[(i + 1) % list.length]
      await bridge.setRate(this.rate)
      this.toast(this.rateLabel)
    },
    async onVolumeChange(val) {
      this.volume = val
      await bridge.setVolume(val)
    },
    syncProgress() {
      if (this.scrubbing || this.durationMs <= 0) return
      this.seekVal = Math.min(this.seekMax, Math.round(this.positionMs / this.durationMs * this.seekMax))
    },
    // 500ms 轮询：进度 + 状态（控制栏隐藏时不刷新 UI，避免运行时重绘覆盖视频）
    async tick() {
      if (!this.path) return
      const pos = await bridge.position()
      const st = await bridge.status()
      this.positionMs = pos
      if (st && st.durationMs > 0) this.durationMs = st.durationMs
      if (st && st.ok) {
        this.playing = st.playing
        if (st.eos) {
          this.ended = true
          this.playing = false
          this.controlsVisible = true
          bridge.setView(WITHBAR)
        }
      }
      if (this.controlsVisible) this.syncProgress()
      this._tickCount = (this._tickCount || 0) + 1
      if (this._tickCount % 6 === 0) {
        history.saveProgress(this.path, pos, this.durationMs).catch(() => {})
      }
    }
  },
  // 本运行时不调用页面根组件的 onLoad：导航参数从 this.$page.options 取（书阁真机验证写法）
  async mounted() {
    const o = (this.$page && this.$page.options) || {}
    this.path = String(o.path || '')
    this.name = String(o.name || '')
    this.rate = Number(o.rate || 1) || 1
    try {
      this.screen = await device.detectScreen()
    } catch (e) { /* 探测失败用默认 800x254 */ }
    let resumeMs = 0
    try {
      const hist = await history.load()
      const it = hist.entries.find(e => e.path === this.path)
      if (it) {
        resumeMs = Number(it.positionMs || 0)
        if (it.volume != null) this.volume = it.volume
        if (it.rate) this.rate = it.rate
      }
    } catch (e) { /* 历史缺失不阻塞播放 */ }
    try {
      if (this.path) await this.start(this.path, resumeMs)
    } catch (e) {
      this.errorText = '播放启动异常\n' + e
    }
    this._pollTimer = setInterval(() => { this.tick().catch(() => {}) }, 500)
    this._hintTimer = setTimeout(() => { this.showHint = false }, 4000)
  },
  async onHide() {
    bridge.pause()
    this.playing = false
    if (this.path) await history.saveProgress(this.path, this.positionMs, this.durationMs).catch(() => {})
  },
  async onUnload() {
    bridge.stop()
    this.playing = false
    if (this._pollTimer) clearInterval(this._pollTimer)
    if (this._hintTimer) clearTimeout(this._hintTimer)
    if (this.path) {
      try {
        await history.record({ path: this.path, name: this.name, positionMs: this.positionMs, durationMs: this.durationMs, rate: this.rate, volume: this.volume })
      } catch (e) { /* 忽略 */ }
    }
  }
}
</script>

<style lang="less" scoped>
@import "../../styles/common.less";
.screen {
  width: 100vw;
  height: 100vh;
  background-color: #000000;
}
.videoarea {
  position: absolute;
  left: 0vw;
  top: 0vh;
  width: 100vw;
  height: 100vh;
}
.controls {
  position: absolute;
  left: 0vw;
  top: 75.59vh;
  width: 100vw;
  height: 24.41vh; /* 62px，视频区收缩到其上方 */
  background-color: #121922;
}
.row1 {
  display: flex;
  flex-direction: row;
  align-items: center;
  padding: 0 1.5vw;
  height: 12vh;
}
.time {
  width: 10vw;
  color: #8ca0ad;
  font-size: 4.2vh;
  text-align: center;
}
.seekbar {
  flex: 1;
  height: 8vh;
}
.row2 {
  display: flex;
  flex-direction: row;
  align-items: center;
  padding: 0 1.5vw;
  height: 12vh;
}
.btn {
  width: 11vw;
  height: 10.5vh;
  background-color: #19242f;
  border-radius: 1vw;
  margin-right: 1vw;
  display: flex;
  align-items: center;
  justify-content: center;
}
.accent {
  background-color: #123a37;
}
.back {
  margin-left: auto;
  margin-right: 0;
}
.btntext {
  color: #e8eef2;
  font-size: 4.4vh;
}
.btntextacc {
  color: #4fd6c3;
  font-size: 4.4vh;
}
.qtag {
  color: #8ca0ad;
  font-size: 3.4vh;
  lines: 1;
  margin-right: 1vw;
}
.volbox {
  display: flex;
  flex-direction: row;
  align-items: center;
  margin-right: 1vw;
}
.volicon {
  color: #f5b85c;
  font-size: 4.4vh;
  width: 4.5vw;
}
.volbar {
  width: 14vw;
  height: 8vh;
}
.voltext {
  color: #8ca0ad;
  font-size: 3.8vh;
  width: 5vw;
  text-align: right;
}
.errbox {
  position: absolute;
  left: 4vw;
  top: 30vh;
  width: 60vw;
}
.errtext {
  color: #ff6b72;
  font-size: 4.4vh;
  line-height: 6.5vh;
}
.hintbox {
  position: absolute;
  left: 30vw;
  top: 80vh;
  width: 40vw;
}
.hinttext {
  color: #8ca0ad;
  font-size: 4vh;
  text-align: center;
}
</style>

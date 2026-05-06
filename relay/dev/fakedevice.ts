/**
 * Fake device — connects to the relay's /relay WebSocket and sends a
 * Hello, then Init, then a Tick every 1.5 s. Used to validate the relay
 * end-to-end without flashing a real ESP32.
 *
 * Usage:
 *   bun run relay/dev/fakedevice.ts                      # connects to the production relay
 *   RELAY_URL=ws://localhost:3000/relay bun run ...      # local dev
 */

const RELAY_URL =
  process.env.RELAY_URL ??
  'wss://project-grasshopper-production.up.railway.app/relay'
const RELAY_TOKEN = process.env.RELAY_TOKEN ?? 'dev-token'
const DEVICE_ID = process.env.DEVICE_ID ?? 'fake-' + Math.floor(Math.random() * 1000)

console.log(`fakedevice: connecting to ${RELAY_URL} as ${DEVICE_ID}`)

const ws = new WebSocket(RELAY_URL)
const bootMs = Date.now()

ws.addEventListener('open', () => {
  console.log('fakedevice: connected, sending hello')
  ws.send(
    JSON.stringify({
      type: 'hello',
      deviceId: DEVICE_ID,
      token: RELAY_TOKEN,
      fwVersion: '0.1.0-fake',
      gitSha: 'deadbeef',
      bootReason: 'cold',
    })
  )

  setTimeout(() => {
    const init = {
      type: 'init',
      fwVersion: '0.1.0-fake',
      gitSha: 'deadbeef',
      deviceId: DEVICE_ID,
      state: 'IDLE',
      uptimeMs: Date.now() - bootMs,
      freeHeap: 180000,
      freePsram: 6_500_000,
      wifi: { mode: 'STA', ssid: 'fakenet', rssi: -45, ip: '192.168.1.50' },
      ntpSynced: true,
      epoch: Math.floor(Date.now() / 1000),
      visible: { fps: 6, w: 1600, h: 1200, quality: 12 },
      thermal: { fps: 9, gain: 'auto', agc: false, spliceDetected: 0, lastFFCMs: 30000 },
    }
    ws.send(JSON.stringify(init))
    console.log('fakedevice: sent init')

    setInterval(() => {
      const tick = {
        type: 'tick',
        uptimeMs: Date.now() - bootMs,
        freeHeap: 180000 + Math.floor(Math.random() * 2000 - 1000),
        freePsram: 6_500_000,
        epoch: Math.floor(Date.now() / 1000),
        state: 'IDLE',
        thermal: {
          fps: 9 + Math.random() - 0.5,
          gain: 'auto',
          agc: false,
          spliceDetected: 0,
          lastFFCMs: 30000,
        },
      }
      ws.send(JSON.stringify(tick))
    }, 1500)

    setInterval(() => {
      ws.send(
        JSON.stringify({
          type: 'log',
          ts: Date.now(),
          level: 'I',
          tag: 'fake',
          msg: 'heartbeat',
        })
      )
    }, 5000)
  }, 250)
})

ws.addEventListener('close', (e) => {
  console.log(`fakedevice: closed code=${e.code} reason=${e.reason}`)
  process.exit(0)
})

ws.addEventListener('error', (e) => {
  console.error('fakedevice: error', e)
})

ws.addEventListener('message', (e) => {
  console.log('fakedevice: rx', e.data.toString().slice(0, 200))
})

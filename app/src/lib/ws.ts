import type { GrasshopperMessage } from '@proto'

// Connect to the device's LAN WebSocket. In dev mode (running `vite`),
// set VITE_WS_URL to point at the device, e.g.
//   VITE_WS_URL=ws://192.168.1.50/ws bun run dev
// or (later, when the relay carries traffic):
//   VITE_WS_URL=wss://relay.example/devices/<id>/ws bun run dev

export function connect(onMessage: (msg: GrasshopperMessage) => void): WebSocket {
  const url = import.meta.env.VITE_WS_URL ?? `ws://${location.host}/ws`
  const ws = new WebSocket(url)

  ws.addEventListener('message', (e) => {
    if (typeof e.data !== 'string') return // binary frames handled elsewhere
    try {
      const msg = JSON.parse(e.data) as GrasshopperMessage
      onMessage(msg)
    } catch (err) {
      console.warn('ws: parse error', err, e.data.slice(0, 200))
    }
  })

  return ws
}

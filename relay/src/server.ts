import { Hono } from 'hono'

const app = new Hono()

app.get('/', (c) =>
  c.text(
    'Grasshopper debug relay — phase 0\n' +
      'Dashboard + WS endpoints arrive in phase 2.\n'
  )
)

app.get('/health', (c) =>
  c.json({
    ok: true,
    service: 'grasshopper-relay',
    version: '0.0.1',
    ts: Date.now(),
  })
)

const port = Number(process.env.PORT ?? 3000)

console.log(`grasshopper-relay listening on :${port}`)

export default {
  port,
  fetch: app.fetch,
}

import { useEffect, useState } from 'preact/hooks'
import { connect } from './lib/ws'
import type { Init, Tick } from '@proto'

type Tab = 'live' | 'capture' | 'library'

export function App() {
  const [tab, setTab] = useState<Tab>('live')
  const [snapshot, setSnapshot] = useState<Partial<Init> | null>(null)
  const [status, setStatus] = useState('connecting…')

  useEffect(() => {
    let cancelled = false
    const ws = connect((msg) => {
      if (cancelled) return
      if (msg.type === 'init') {
        setSnapshot(msg as Init)
        setStatus('online')
      } else if (msg.type === 'tick') {
        setSnapshot((s) => ({ ...(s ?? {}), ...(msg as Tick) }))
      }
    })
    ws.addEventListener('open', () => setStatus('connected, awaiting init'))
    ws.addEventListener('close', () => setStatus('disconnected'))
    ws.addEventListener('error', () => setStatus('error'))
    return () => {
      cancelled = true
      ws.close()
    }
  }, [])

  return (
    <main>
      <header>
        <h1>🦗 Grasshopper</h1>
        <span class="status">{status}</span>
      </header>
      <nav>
        <button class={tab === 'live' ? 'active' : ''} onClick={() => setTab('live')}>Live</button>
        <button class={tab === 'capture' ? 'active' : ''} onClick={() => setTab('capture')}>Capture</button>
        <button class={tab === 'library' ? 'active' : ''} onClick={() => setTab('library')}>Library</button>
      </nav>
      <section>
        {tab === 'live' && (
          <>
            <p class="hint">Phase 2 stub — full Live view (camera + thermal + controls) lands in phase 7.</p>
            <pre class="snapshot">
              {snapshot ? JSON.stringify(snapshot, null, 2) : '— awaiting first message —'}
            </pre>
          </>
        )}
        {tab === 'capture' && <p class="hint">Capture controls land in phase 5.</p>}
        {tab === 'library' && <p class="hint">Session library lands in phase 7.</p>}
      </section>
    </main>
  )
}

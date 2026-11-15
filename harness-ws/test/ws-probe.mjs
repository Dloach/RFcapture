/**
 * End-to-end probe for the Cardputer WebSocket surface.
 *
 *   node ws-probe.mjs list
 *   node ws-probe.mjs open <sessionId>
 *   node ws-probe.mjs prompt <sessionId> <text> [timeoutMs]
 *   node ws-probe.mjs watch <sessionId> [timeoutMs]
 *
 * Uses Node's built-in WebSocket client, so it needs no dependency.
 */
const [, , mode, ...rest] = process.argv

const url = process.env.WS_URL ?? 'ws://127.0.0.1:9101/cardputer?token=cardputer-dev'
const timeoutMs = Number(process.env.WS_TIMEOUT_MS ?? 0) || (Number(rest[2] ?? rest[1]) || 0) || 20000

const stamp = () => new Date().toISOString().slice(11, 23)
const out = value => console.log(`${stamp()} ${JSON.stringify(value)}`)

const socket = new WebSocket(url)
let settled = false
const finish = code => {
  if (settled) return
  settled = true
  try {
    socket.close()
  } catch {}
  setTimeout(() => process.exit(code), 50)
}
const watchdog = setTimeout(() => {
  out({ event: 'timeout', afterMs: timeoutMs })
  finish(0)
}, timeoutMs)

let deltaCount = 0
let deltaChars = 0
const deltaStart = { at: 0 }

socket.onopen = () => {
  out({ event: 'open', url })
}

socket.onerror = event => {
  out({ event: 'error', message: String(event?.message ?? event) })
  clearTimeout(watchdog)
  finish(1)
}

socket.onclose = event => {
  out({ event: 'close', code: event.code, reason: event.reason })
  clearTimeout(watchdog)
  finish(0)
}

socket.onmessage = event => {
  let frame
  try {
    frame = JSON.parse(event.data)
  } catch {
    out({ event: 'bad-frame', raw: String(event.data).slice(0, 120) })
    return
  }

  if (frame.type === 'delta') {
    deltaCount += 1
    deltaChars += frame.text.length
    if (deltaCount === 1) {
      deltaStart.at = Date.now()
      out({ event: 'first-delta', text: frame.text })
    }
    return
  }

  if (frame.type === 'sessions') {
    out({
      event: 'sessions',
      count: frame.items.length,
      items: frame.items.slice(0, 8).map(item => ({
        id: item.shortId,
        title: item.title,
        preview: item.preview,
        running: item.running,
        updatedAt: new Date(item.updatedAt).toISOString(),
      })),
    })
    clearTimeout(watchdog)
    finish(0)
    return
  }

  if (frame.type === 'snapshot') {
    out({
      event: 'snapshot',
      sessionId: frame.sessionId.slice(0, 8),
      cursor: frame.cursor,
      messages: frame.messages.length,
      tail: frame.messages.slice(-3).map(m => ({ role: m.role, text: m.text.slice(0, 90) })),
    })
    if (mode === 'open') {
      clearTimeout(watchdog)
      finish(0)
      return
    }
    if (mode === 'create' || mode === 'prompt' || mode === 'cancel') {
      const text = mode === 'create'
        ? (rest[0] ?? 'probe: reply with the single word OK')
        : (rest[1] ?? 'probe: reply with the single word OK')
      if (mode === 'create' && frame.messages.length > 0) return
      out({ event: 'sending-prompt', text })
      process.env.PROMPT_SENT_AT = String(Date.now())
      socket.send(JSON.stringify({ type: 'prompt', sessionId: frame.sessionId, text }))
      if (mode === 'cancel') {
        setTimeout(() => {
          out({ event: 'sending-cancel' })
          socket.send(JSON.stringify({ type: 'cancel', sessionId: frame.sessionId }))
        }, 1200)
      }
    }
    return
  }

  if (frame.type === 'message') {
    out({
      event: 'message',
      role: frame.role,
      seq: frame.seq,
      text: frame.text.slice(0, 200),
      deltasBefore: deltaCount,
      deltaChars,
    })
    if (mode === 'prompt' && frame.role === 'assistant') {
      out({ event: 'summary', deltaFrames: deltaCount, deltaChars, msToFirstDelta: deltaStart.at ? deltaStart.at - Number(process.env.PROMPT_SENT_AT ?? 0) : null })
      clearTimeout(watchdog)
      finish(0)
    }
    return
  }

  out({ event: 'frame', frame })
  if (frame.type === 'hello') {
    if (mode === 'list') socket.send(JSON.stringify({ type: 'sessions' }))
    if (mode === 'open' || mode === 'watch') {
      socket.send(JSON.stringify({ type: 'open', sessionId: rest[0] }))
    }
    if (mode === 'prompt' || mode === 'cancel') {
      socket.send(JSON.stringify({ type: 'open', sessionId: rest[0] }))
    }
    if (mode === 'create') {
      socket.send(JSON.stringify({ type: 'create' }))
    }
  }
}

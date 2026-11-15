/**
 * dsh-cardputer-ws — LAN WebSocket surface for the M5Stack Cardputer.
 *
 * Unlike the ACP/HTTP relay this replaces, the plugin runs inside the Desktop
 * Host and drives the *same* live Sessions the Web UI uses through
 * `ctx.sessionController`. There is no second Agent and no separate Session:
 *
 *   Cardputer  <-- ws://host:9101/cardputer -->  this plugin  -->  ctx.sessionController
 *                                                                        |
 *                                                            Desktop Web UI (same Session)
 *
 * Protocol (JSON text frames), documented in docs/DETAIL.md.
 *
 * Revision 3: resolves `sessionController` only through `ctx.get`, and no HTTP
 * or upgrade callback can throw into the Host process.
 */
import http from 'node:http'
import { spawn } from 'node:child_process'
import { appendFileSync, unlinkSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'

import { attachWebSocket } from './ws.js'

const HERE = dirname(fileURLToPath(import.meta.url))

const DEFAULTS = {
  host: '0.0.0.0',
  port: 9101,
  path: '/cardputer',
  token: 'cardputer-dev',
  allowNonLan: false,
  workspace: '',
  previewLimit: 10,
  listLimit: 20,
  maxMessages: 40,
  deltaFlushMs: 60,
  sttPython: '',
  sttScript: join(HERE, 'stt.py'),
  sttModel: '',
  logFile: join(tmpdir(), 'dsh-cardputer-ws.log'),
}

const MAX_STT_BYTES = 2 * 1024 * 1024

// ---------------------------------------------------------------------------
// helpers

function normalize(config) {
  const raw = config && typeof config === 'object' ? config : {}
  const cfg = { ...DEFAULTS }
  for (const [key, value] of Object.entries(raw)) {
    if (value === undefined || value === null || value === '') continue
    cfg[key] = value
  }
  cfg.port = Number(cfg.port) || DEFAULTS.port
  cfg.previewLimit = Math.max(0, Math.min(50, Number(cfg.previewLimit) || 0))
  cfg.listLimit = Math.max(1, Math.min(40, Number(cfg.listLimit) || DEFAULTS.listLimit))
  cfg.maxMessages = Math.max(2, Math.min(200, Number(cfg.maxMessages) || DEFAULTS.maxMessages))
  cfg.deltaFlushMs = Math.max(16, Math.min(1000, Number(cfg.deltaFlushMs) || DEFAULTS.deltaFlushMs))
  if (typeof cfg.path !== 'string' || !cfg.path.startsWith('/')) cfg.path = DEFAULTS.path
  cfg.allowNonLan = cfg.allowNonLan === true || cfg.allowNonLan === 'true'
  return cfg
}

function createLogger(cfg) {
  const file = typeof cfg.logFile === 'string' && cfg.logFile.length > 0 ? cfg.logFile : ''
  const write = (level, message) => {
    if (!file) return
    try {
      appendFileSync(file, `${new Date().toISOString()} ${level} ${message}\n`)
    } catch {}
  }
  return {
    info: message => write('INFO', message),
    warn: message => write('WARN', message),
    file,
  }
}

function shortId(sessionId) {
  if (typeof sessionId !== 'string') return ''
  // Ordinary Session ids are `session-<uuid>`; the leading word carries no
  // information on a 30-column display.
  return sessionId.replace(/^session-/, '').slice(0, 8)
}

/** Join the text blocks of one message's content. */
function textOfContent(content) {
  if (typeof content === 'string') return content
  if (!Array.isArray(content)) return ''
  let text = ''
  for (const part of content) {
    if (part && part.type === 'text' && typeof part.text === 'string') text += part.text
  }
  return text
}

/**
 * Runtime context the Harness injects as user turns. The Desktop transcript
 * hides it, and a 240x135 screen cannot afford it.
 */
function looksLikeRuntimeContext(text) {
  const trimmed = text.trimStart()
  return (
    trimmed.startsWith('<system-reminder>') ||
    trimmed.startsWith('Time sampled while preparing turn')
  )
}

/** Map one committed Session record to a chat message, or null. */
function messageOfRecord(record) {
  const event = record && typeof record === 'object' && 'event' in record ? record.event : record
  if (!event || typeof event.type !== 'string') return null
  if (event.type !== 'user/message' && event.type !== 'assistant/message') return null
  const data = event.data ?? {}
  // A user turn carries `data.content`; an assistant turn nests the model
  // message, so its blocks live one level down at `data.message.content`.
  const content = data.content ?? (data.message && data.message.content)
  const text = textOfContent(content)
  if (text.trim().length === 0) return null
  const role = event.type === 'user/message' ? 'user' : 'assistant'
  if (role === 'user' && looksLikeRuntimeContext(text)) return null
  return {
    seq: typeof event.seq === 'number' ? event.seq : 0,
    role,
    text,
  }
}

function collapse(text, limit) {
  const flat = String(text).replace(/\s+/g, ' ').trim()
  return flat.length > limit ? `${flat.slice(0, limit - 1)}…` : flat
}

/** True for loopback, RFC1918, and link-local peers. */
function isLanAddress(address) {
  if (typeof address !== 'string' || address.length === 0) return false
  let value = address
  if (value.startsWith('::ffff:')) value = value.slice(7)
  if (value === '::1' || value === '127.0.0.1') return true
  if (value.startsWith('fe80:') || value.startsWith('fc') || value.startsWith('fd')) return true
  const parts = value.split('.')
  if (parts.length !== 4) return false
  const octets = parts.map(Number)
  if (octets.some(n => !Number.isInteger(n) || n < 0 || n > 255)) return false
  if (octets[0] === 10) return true
  if (octets[0] === 127) return true
  if (octets[0] === 192 && octets[1] === 168) return true
  if (octets[0] === 172 && octets[1] >= 16 && octets[1] <= 31) return true
  if (octets[0] === 169 && octets[1] === 254) return true
  return false
}

// ---------------------------------------------------------------------------
// one connected Cardputer

class CardputerClient {
  constructor(connection, plugin) {
    this.connection = connection
    this.plugin = plugin
    this.sessionId = null
    this.followAbort = null
    this.followActive = false
    this.deltaBuffer = ''
    this.deltaTimer = null
    this.stt = null
    this.disposed = false
    this.lastSeen = Date.now()

    connection.onmessage = text => {
      this.lastSeen = Date.now()
      this.receive(text).catch(error => this.fail(error))
    }
    // Protocol-level ping/pong counts as life too: the firmware pings every
    // 20 s, and it does not answer the application-level `ping` below.
    connection.onactivity = () => {
      this.lastSeen = Date.now()
    }
    connection.onclose = () => this.dispose()

    // A device that loses power never sends FIN, so its socket and follow
    // subscription would stay alive forever. Ping it and drop the silent ones.
    this.keepAlive = setInterval(() => {
      if (this.disposed) return
      if (Date.now() - this.lastSeen > 45000) {
        this.plugin.log.info(`client timeout ${this.connection.meta.remoteAddress ?? '?'}`)
        this.dispose()
        this.connection.destroy()
        return
      }
      this.send({ type: 'ping' })
    }, 15000)

    this.send({ type: 'hello', protocol: 1, name: 'dsh-cardputer-ws' })
  }

  send(frame) {
    if (this.disposed) return
    try {
      this.connection.send(JSON.stringify(frame))
    } catch {}
  }

  fail(error) {
    this.send({ type: 'error', error: String((error && error.message) || error) })
  }

  async receive(text) {
    let frame
    try {
      frame = JSON.parse(text)
    } catch {
      this.send({ type: 'error', error: 'bad json' })
      return
    }
    if (!frame || typeof frame.type !== 'string') {
      this.send({ type: 'error', error: 'missing type' })
      return
    }

    switch (frame.type) {
      case 'ping':
        this.send({ type: 'pong', time: Date.now() })
        return
      case 'pong':
        // The firmware answers our keepalive; nothing to do, and it must not
        // be reported as an unknown message type.
        return
      case 'sessions':
        await this.sendSessions(frame.limit)
        return
      case 'open':
        await this.openSession(frame.sessionId, frame.maxMessages)
        return
      case 'create':
        await this.createSession(frame.cwd)
        return
      case 'prompt':
        await this.prompt(frame.sessionId, frame.text)
        return
      case 'cancel':
        await this.cancel(frame.sessionId)
        return
      case 'stt.begin':
        this.sttBegin(frame)
        return
      case 'stt.chunk':
        this.sttChunk(frame.data)
        return
      case 'stt.end':
        await this.sttEnd()
        return
      case 'stt.abort':
        this.stt = null
        this.send({ type: 'stt.error', error: 'cancelled' })
        return
      default:
        this.send({ type: 'error', error: `unknown type ${frame.type}` })
    }
  }

  controller() {
    // `ctx.sessionController` throws in Cordis unless the plugin declares
    // `inject`; the generic accessor is always legal and returns undefined when
    // the service is absent, so nothing here can throw during activation.
    const ctx = this.plugin.ctx
    if (typeof ctx.get === 'function') return ctx.get('sessionController')
    return undefined
  }

  requireController() {
    const controller = this.controller()
    if (!controller) throw new Error('sessionController unavailable')
    return controller
  }

  // -- sessions -------------------------------------------------------------

  async sendSessions(limit) {
    const controller = this.requireController()
    const signal = new AbortController().signal
    const value = await controller.list({}, signal)
    const items = Array.isArray(value && value.items) ? value.items : []
    const max = Math.max(1, Math.min(40, Number(limit) || this.plugin.cfg.listLimit))
    const rows = items.slice(0, max).map(item => {
      const projections = item.projections && item.projections.values ? item.projections.values : {}
      const title = typeof projections.title === 'string' ? projections.title : ''
      return {
        id: String(item.sessionId),
        shortId: shortId(item.sessionId),
        title,
        preview: '',
        updatedAt: Number(item.updatedAt) || 0,
        running: item.running === true,
        blank: item.blank === true,
        cwd: typeof item.cwd === 'string' ? item.cwd : '',
      }
    })

    // Title/preview enrichment costs one history read per Session, so only the
    // most recent few are filled in and the result is cached per updatedAt.
    const candidates = rows.filter(row => !row.blank && row.updatedAt > 0).slice(0, this.plugin.cfg.previewLimit)
    await Promise.all(candidates.map(row => this.enrich(row, signal).catch(() => {})))
    this.send({ type: 'sessions', items: rows })
  }

  async enrich(row, signal) {
    const cached = this.plugin.previewCache.get(row.id)
    if (cached && cached.updatedAt === row.updatedAt) {
      row.preview = cached.preview
      if (row.title.length === 0) row.title = cached.title
      return
    }
    const controller = this.requireController()
    const page = await controller.events(
      { address: { kind: 'session', sessionId: row.id }, maxMessages: 16 },
      signal,
    )
    const messages = (Array.isArray(page && page.records) ? page.records : [])
      .map(messageOfRecord)
      .filter(Boolean)
    const last = messages[messages.length - 1]
    const firstUser = messages.find(message => message.role === 'user')
    const preview = last ? collapse(last.text, 64) : ''
    const title = row.title.length > 0 ? row.title : firstUser ? collapse(firstUser.text, 48) : ''
    this.plugin.previewCache.set(row.id, { updatedAt: row.updatedAt, preview, title })
    row.preview = preview
    row.title = title
  }

  // -- follow ---------------------------------------------------------------

  stopFollow() {
    if (this.followAbort) {
      try {
        this.followAbort.abort()
      } catch {}
      this.followAbort = null
    }
    this.followActive = false
    this.flushDelta()
  }

  async openSession(sessionId, maxMessages) {
    if (typeof sessionId !== 'string' || sessionId.length === 0) {
      this.send({ type: 'error', error: 'sessionId required' })
      return
    }
    this.stopFollow()
    this.sessionId = sessionId
    this.send({ type: 'opened', sessionId })

    const controller = this.requireController()
    const state = await controller.state({ sessionId }, new AbortController().signal).catch(() => null)
    this.send({ type: 'status', sessionId, running: state ? state.running === true : false })
    const messages = Math.max(4, Math.min(60, Number(maxMessages) || this.plugin.cfg.maxMessages))
    this.startFollow(sessionId, messages)
  }

  startFollow(sessionId, maxMessages) {
    const controller = this.requireController()
    const abort = new AbortController()
    this.followAbort = abort
    this.followActive = true
    this.plugin.track(abort)

    const request = {
      address: { kind: 'session', sessionId },
      assistantStream: true,
      maxMessages,
    }

    let iterator
    try {
      iterator = controller.follow(request, abort.signal)
    } catch (error) {
      this.followActive = false
      this.plugin.untrack(abort)
      this.fail(error)
      return
    }

    ;(async () => {
      try {
        for await (const frame of iterator) {
          if (abort.signal.aborted || this.disposed) break
          this.onFollowFrame(sessionId, frame)
        }
      } catch (error) {
        if (!abort.signal.aborted && !this.disposed) {
          this.send({ type: 'error', error: `follow: ${String((error && error.message) || error)}` })
        }
      } finally {
        this.plugin.untrack(abort)
        // Only the newest follow may clear the flag; an older one finishing
        // after a re-open must not make this client look unwatched.
        if (this.followAbort === abort) this.followActive = false
      }
    })()
  }

  onFollowFrame(sessionId, frame) {
    if (!frame || typeof frame !== 'object') return

    if (frame.type === 'snapshot') {
      const messages = (Array.isArray(frame.records) ? frame.records : [])
        .map(messageOfRecord)
        .filter(Boolean)
      this.send({
        type: 'snapshot',
        sessionId,
        cursor: typeof frame.cursor === 'number' ? frame.cursor : 0,
        messages,
      })
      return
    }

    if (frame.type === 'event') {
      const message = messageOfRecord(frame)
      if (message) {
        this.send({
          type: 'message',
          sessionId,
          seq: message.seq,
          role: message.role,
          text: message.text,
        })
        // The prompt has been committed: a live Agent now exists for this
        // Session, so refresh the cached card data on the next list.
        this.plugin.previewCache.delete(sessionId)
      }
      return
    }

    if (frame.type === 'assistant-stream' && frame.frame) {
      const inner = frame.frame
      if (inner.type === 'start') {
        this.send({ type: 'status', sessionId, running: true })
        return
      }
      if (inner.type === 'end') {
        this.flushDelta()
        this.send({ type: 'status', sessionId, running: false })
        return
      }
      if (inner.type === 'chunk') {
        const chunk = inner.chunk
        if (chunk && chunk.type === 'text-delta' && typeof chunk.text === 'string' && chunk.text.length > 0) {
          this.queueDelta(sessionId, chunk.text)
        }
      }
    }
  }

  // -- delta coalescing -----------------------------------------------------

  queueDelta(sessionId, text) {
    this.deltaSession = sessionId
    this.deltaBuffer += text
    if (this.deltaBuffer.length >= 512) {
      this.flushDelta()
      return
    }
    if (this.deltaTimer === null) {
      this.deltaTimer = setTimeout(() => {
        this.deltaTimer = null
        this.flushDelta()
      }, this.plugin.cfg.deltaFlushMs)
    }
  }

  flushDelta() {
    if (this.deltaTimer !== null) {
      clearTimeout(this.deltaTimer)
      this.deltaTimer = null
    }
    if (this.deltaBuffer.length === 0) return
    const text = this.deltaBuffer
    this.deltaBuffer = ''
    this.send({ type: 'delta', sessionId: this.deltaSession ?? this.sessionId, text })
  }

  // -- actions --------------------------------------------------------------

  async createSession(cwd) {
    const controller = this.requireController()
    const request = {}
    const target = typeof cwd === 'string' && cwd.length > 0 ? cwd : this.plugin.cfg.workspace
    if (typeof target === 'string' && target.length > 0) request.cwd = target
    const value = await controller.create(request)
    const sessionId = String(value.sessionId)
    this.send({ type: 'created', sessionId, shortId: shortId(sessionId) })
    await this.openSession(sessionId)
  }

  async prompt(sessionId, text) {
    if (typeof text !== 'string' || text.trim().length === 0) {
      this.send({ type: 'error', error: 'text required' })
      return
    }
    const target = typeof sessionId === 'string' && sessionId.length > 0 ? sessionId : this.sessionId
    if (!target) {
      this.send({ type: 'error', error: 'sessionId required' })
      return
    }
    const controller = this.requireController()
    const value = await controller.prompt(
      {
        requestId: `cardputer-${Date.now()}-${Math.random().toString(36).slice(2, 10)}`,
        sessionId: target,
        mode: 'queue',
        content: [{ type: 'text', text }],
      },
      new AbortController().signal,
    )
    this.send({ type: 'accepted', sessionId: target, accepted: value ? value.accepted === true : false })
    // Sending can resume a cold Session into a live Agent; if the earlier
    // follow had already ended, watch the now-live log again.
    if (!this.followActive) this.startFollow(target)
  }

  async cancel(sessionId) {
    const target = typeof sessionId === 'string' && sessionId.length > 0 ? sessionId : this.sessionId
    if (!target) {
      this.send({ type: 'error', error: 'sessionId required' })
      return
    }
    const controller = this.requireController()
    const value = controller.cancel({ sessionId: target })
    this.send({ type: 'cancelled', sessionId: target, accepted: value ? value.accepted === true : false })
  }

  // -- speech to text -------------------------------------------------------

  sttBegin(frame) {
    this.stt = {
      rate: Number(frame && frame.rate) || 16000,
      channels: Number(frame && frame.channels) || 1,
      bits: Number(frame && frame.bits) || 16,
      chunks: [],
      bytes: 0,
    }
    this.send({ type: 'stt.ready' })
  }

  sttChunk(data) {
    if (!this.stt) {
      this.send({ type: 'stt.error', error: 'stt not started' })
      return
    }
    let buffer
    try {
      buffer = Buffer.from(String(data || ''), 'base64')
    } catch {
      this.send({ type: 'stt.error', error: 'bad base64' })
      return
    }
    this.stt.bytes += buffer.length
    if (this.stt.bytes > MAX_STT_BYTES) {
      this.stt = null
      this.send({ type: 'stt.error', error: 'audio too large' })
      return
    }
    this.stt.chunks.push(buffer)
  }

  async sttEnd() {
    const session = this.stt
    this.stt = null
    if (!session || session.bytes === 0) {
      this.send({ type: 'stt.error', error: 'no audio' })
      return
    }
    const cfg = this.plugin.cfg
    if (!cfg.sttPython || !cfg.sttModel) {
      this.send({ type: 'stt.error', error: 'stt not configured' })
      return
    }

    const pcm = Buffer.concat(session.chunks)
    session.chunks = []
    const wav = Buffer.allocUnsafe(44 + pcm.length)
    const byteRate = (session.rate * session.channels * session.bits) / 8
    const blockAlign = (session.channels * session.bits) / 8
    wav.write('RIFF', 0, 'ascii')
    wav.writeUInt32LE(36 + pcm.length, 4)
    wav.write('WAVE', 8, 'ascii')
    wav.write('fmt ', 12, 'ascii')
    wav.writeUInt32LE(16, 16)
    wav.writeUInt16LE(1, 20)
    wav.writeUInt16LE(session.channels, 22)
    wav.writeUInt32LE(session.rate, 24)
    wav.writeUInt32LE(byteRate, 28)
    wav.writeUInt16LE(blockAlign, 32)
    wav.writeUInt16LE(session.bits, 34)
    wav.write('data', 36, 'ascii')
    wav.writeUInt32LE(pcm.length, 40)
    pcm.copy(wav, 44)

    const result = await this.plugin.transcribe(wav, session.rate)
    if (result.error) this.send({ type: 'stt.error', error: result.error })
    else this.send({ type: 'stt', text: result.text })
  }

  dispose() {
    if (this.disposed) return
    this.disposed = true
    if (this.keepAlive) {
      clearInterval(this.keepAlive)
      this.keepAlive = null
    }
    this.stopFollow()
    this.stt = null
  }
}

// ---------------------------------------------------------------------------
// plugin

export function apply(ctx, config) {
  const cfg = normalize(config)
  const log = createLogger(cfg)
  log.info(`apply port=${cfg.port} path=${cfg.path}`)

  const clients = new Set()
  const cleanups = new Set()
  const previewCache = new Map()

  const plugin = {
    ctx,
    cfg,
    log,
    previewCache,
    track(abort) {
      cleanups.add(abort)
      return () => cleanups.delete(abort)
    },
    untrack(abort) {
      cleanups.delete(abort)
    },
    transcribe(wav, rate) {
      return new Promise(resolve => {
        if (!cfg.sttPython || !cfg.sttModel) {
          resolve({ error: 'stt not configured' })
          return
        }
        const file = join(tmpdir(), `cardputer-stt-${Date.now()}-${Math.random().toString(36).slice(2, 8)}.wav`)
        try {
          writeFileSync(file, wav)
        } catch (error) {
          resolve({ error: `stt write: ${String((error && error.message) || error)}` })
          return
        }
        const cleanup = () => {
          try {
            unlinkSync(file)
          } catch {}
        }
        let child
        try {
          child = spawn(cfg.sttPython, [cfg.sttScript, cfg.sttModel, file], {
            windowsHide: true,
            // Same reason as stt.py reconfiguring stdout: never let the child
            // inherit a GBK/cp936 console encoding for its pipe.
            env: { ...process.env, PYTHONIOENCODING: 'utf-8', PYTHONUTF8: '1' },
          })
        } catch (error) {
          cleanup()
          resolve({ error: `stt spawn: ${String((error && error.message) || error)}` })
          return
        }
        let stdout = ''
        let stderr = ''
        let settled = false
        const finish = value => {
          if (settled) return
          settled = true
          clearTimeout(timer)
          cleanup()
          resolve(value)
        }
        const timer = setTimeout(() => {
          try {
            child.kill()
          } catch {}
          finish({ error: 'stt timeout' })
        }, 30000)
        child.stdout.on('data', chunk => {
          stdout += chunk.toString('utf8')
        })
        child.stderr.on('data', chunk => {
          stderr += chunk.toString('utf8')
        })
        child.on('error', error => {
          finish({ error: `stt error: ${String((error && error.message) || error)}` })
        })
        child.on('close', code => {
          if (code !== 0) {
            finish({ error: `stt exit ${code}: ${stderr.trim().slice(0, 160)}` })
            return
          }
          finish({ text: stdout.trim(), rate })
        })
      })
    },
  }

  const server = http.createServer((request, response) => {
    const headers = { 'cache-control': 'no-store' }
    const send = (status, body) => {
      try {
        const payload = Buffer.from(JSON.stringify(body))
        headers['content-type'] = 'application/json; charset=utf-8'
        headers['content-length'] = String(payload.length)
        response.writeHead(status, headers)
        response.end(payload)
      } catch {}
    }
    // A throw here is an uncaught exception in the Desktop Host process, so the
    // whole handler is contained.
    try {
      const url = new URL(request.url ?? '/', 'http://cardputer.local')
      if (url.pathname !== '/cardputer/health') {
        send(404, { error: 'not found' })
        return
      }
      const token = url.searchParams.get('token') ?? request.headers['x-cardputer-token']
      if (token !== cfg.token) {
        send(401, { error: 'bad token' })
        return
      }
      const controller = typeof ctx.get === 'function' ? ctx.get('sessionController') : undefined
      send(200, {
        ok: true,
        name: 'dsh-cardputer-ws',
        port: cfg.port,
        path: cfg.path,
        clients: clients.size,
        sessions: Boolean(controller),
        stt: Boolean(cfg.sttPython && cfg.sttModel),
      })
    } catch (error) {
      log.warn(`health failed: ${String((error && error.message) || error)}`)
      send(500, { error: 'internal' })
    }
  })

  server.on('error', error => {
    log.warn(`listen failed: ${String((error && error.message) || error)}`)
  })

  attachWebSocket(server, {
    path: cfg.path,
    authorize(request, url) {
      if (url.searchParams.get('token') !== cfg.token) {
        const header = request.headers['x-cardputer-token']
        const authorization = request.headers.authorization
        const bearer = typeof authorization === 'string' && authorization.startsWith('Bearer ')
          ? authorization.slice(7)
          : undefined
        if (header !== cfg.token && bearer !== cfg.token) return 'bad token'
      }
      if (!cfg.allowNonLan && !isLanAddress(request.socket.remoteAddress)) return 'lan only'
      return null
    },
    onConnection(connection) {
      const client = new CardputerClient(connection, plugin)
      clients.add(client)
      connection.onclose = () => {
        clients.delete(client)
        client.dispose()
        log.info(`client gone ${connection.meta.remoteAddress ?? '?'} total=${clients.size}`)
      }
      log.info(`client connected ${connection.meta.remoteAddress ?? '?'} total=${clients.size}`)
    },
  })

  server.listen(cfg.port, cfg.host, () => {
    log.info(`listening ws://${cfg.host}:${cfg.port}${cfg.path}`)
  })

  ctx.effect(() => {
    return () => {
      log.info('dispose')
      for (const client of clients) {
        try {
          client.connection.close(1001)
        } catch {}
        client.dispose()
      }
      clients.clear()
      for (const abort of cleanups) {
        try {
          abort.abort()
        } catch {}
      }
      cleanups.clear()
      try {
        server.close()
      } catch {}
    }
  })
}

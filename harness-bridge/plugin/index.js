import http from 'node:http'
import { randomUUID } from 'node:crypto'
import { appendFileSync, writeFileSync, unlinkSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { spawnSync } from 'node:child_process'

const LOG = 'C:/Users/dloach/.dsh/cardputer-bridge.log'
function log(msg) {
  try { appendFileSync(LOG, `${new Date().toISOString()} ${msg}\n`) } catch {}
}
log('module loaded')

const DEFAULTS = {
  host: '0.0.0.0',
  port: 8765,
  token: 'cardputer-dev',
  workspace: process.cwd(),
  python: '',
  sttScript: '',
  sttModel: '',
}

function textOf(content) {
  if (typeof content === 'string') return content
  if (!Array.isArray(content)) return ''
  return content
    .filter(part => part && part.type === 'text' && typeof part.text === 'string')
    .map(part => part.text)
    .join('')
}

function messageOf(record) {
  if (!record || record.type !== 'event' || !record.event) return null
  const event = record.event
  if (event.type !== 'user/message' && event.type !== 'assistant/message') return null
  const text = textOf(event.data?.content)
  if (!text) return null
  return {
    seq: event.seq,
    role: event.type === 'user/message' ? 'user' : 'assistant',
    text,
    time: event.time,
  }
}

function json(res, code, value) {
  const body = Buffer.from(JSON.stringify(value))
  res.writeHead(code, {
    'content-type': 'application/json; charset=utf-8',
    'content-length': body.length,
    'cache-control': 'no-store',
  })
  res.end(body)
}

async function readBody(req, limit = 8 * 1024 * 1024) {
  const chunks = []
  let size = 0
  for await (const chunk of req) {
    size += chunk.length
    if (size > limit) throw new Error('body too large')
    chunks.push(chunk)
  }
  return Buffer.concat(chunks)
}

export function apply(rootCtx, config) {
  const cfg = { ...DEFAULTS, ...config }
  log(`apply host=${cfg.host} port=${cfg.port} effect=${typeof rootCtx.effect}`)

  const getSessionController = () =>
    rootCtx.sessionController ?? rootCtx.get?.('sessionController')

  const server = http.createServer(async (req, res) => {
    const sc = getSessionController()
    if (!sc) {
      json(res, 503, { error: 'sessionController unavailable' })
      return
    }
    try {
      const url = new URL(req.url ?? '/', 'http://bridge.local')
      if (req.method === 'OPTIONS') {
        res.writeHead(204, {
          'access-control-allow-origin': '*',
          'access-control-allow-headers': 'content-type, x-cardputer-token',
          'access-control-allow-methods': 'GET, POST, OPTIONS',
        })
        res.end()
        return
      }
      if (url.pathname === '/cardputer/health') {
        json(res, 200, { ok: true, name: 'dsh-cardputer-bridge' })
        return
      }
      if (req.headers['x-cardputer-token'] !== cfg.token) {
        json(res, 401, { error: 'bad token' })
        return
      }

      const signal = new AbortController().signal
      if (url.pathname === '/cardputer/sessions' && req.method === 'GET') {
        const value = await sc.list({}, signal)
        const items = (value.items ?? []).map(item => ({
          sessionId: item.sessionId,
          updatedAt: item.updatedAt,
          running: item.running,
          blank: item.blank,
          cwd: item.cwd,
          origin: item.origin,
        }))
        json(res, 200, { sessions: items })
        return
      }

      if (url.pathname === '/cardputer/new' && req.method === 'POST') {
        const body = JSON.parse((await readBody(req)).toString('utf8') || '{}')
        const value = await sc.create({ cwd: body.cwd || cfg.workspace }, signal)
        json(res, 200, { sessionId: value.sessionId })
        return
      }

      if (url.pathname === '/cardputer/messages' && req.method === 'GET') {
        const sessionId = url.searchParams.get('session')
        if (!sessionId) return json(res, 400, { error: 'session required' })
        const limit = Math.max(1, Math.min(120, Number(url.searchParams.get('limit') || 60)))
        const page = await sc.events(
          { address: { kind: 'session', sessionId }, maxMessages: limit },
          signal,
        )
        const messages = (page.records ?? []).map(messageOf).filter(Boolean)
        let running = false
        try {
          running = (await sc.state({ sessionId }, signal)).running
        } catch {}
        json(res, 200, {
          sessionId,
          cursor: page.cursor,
          running,
          messages,
        })
        return
      }

      if (url.pathname === '/cardputer/prompt' && req.method === 'POST') {
        const body = JSON.parse((await readBody(req)).toString('utf8') || '{}')
        if (!body.session || typeof body.text !== 'string' || !body.text.trim()) {
          return json(res, 400, { error: 'session and text required' })
        }
        const value = await sc.prompt(
          {
            sessionId: body.session,
            requestId: randomUUID(),
            mode: 'queue',
            content: [{ type: 'text', text: body.text }],
          },
          signal,
        )
        json(res, 200, value)
        return
      }

      if (url.pathname === '/cardputer/cancel' && req.method === 'POST') {
        const body = JSON.parse((await readBody(req)).toString('utf8') || '{}')
        if (!body.session) return json(res, 400, { error: 'session required' })
        json(res, 200, sc.cancel({ sessionId: body.session }))
        return
      }

      if (url.pathname === '/cardputer/stt' && req.method === 'POST') {
        if (!cfg.python || !cfg.sttScript || !cfg.sttModel) {
          return json(res, 503, { error: 'STT not configured' })
        }
        const body = await readBody(req)
        const wav = join(tmpdir(), `cardputer-${randomUUID()}.wav`)
        writeFileSync(wav, body)
        try {
          const p = spawnSync(cfg.python, [cfg.sttScript, cfg.sttModel, wav], {
            encoding: 'utf8',
            timeout: 30000,
            windowsHide: true,
          })
          if (p.status !== 0) {
            return json(res, 500, { error: 'stt failed', status: p.status, stderr: p.stderr?.slice(0, 300) })
          }
          json(res, 200, { text: (p.stdout || '').trim() })
        } finally {
          try { unlinkSync(wav) } catch {}
        }
        return
      }

      json(res, 404, { error: 'not found' })
    } catch (error) {
      rootCtx.logger?.warn?.(`cardputer-bridge: ${error?.stack ?? error}`)
      json(res, 500, { error: String(error?.message ?? error) })
    }
  })

  server.on('error', (error) => {
    log(`listen failed ${error?.message ?? error}`)
    rootCtx.logger?.warn?.(`cardputer-bridge listen failed: ${error?.message ?? error}`)
  })

  rootCtx.effect(() => {
    log('effect start')
    const timer = setTimeout(() => {
      log('calling listen')
      server.listen(cfg.port, cfg.host, () => {
        log(`listening on ${cfg.host}:${cfg.port}`)
        rootCtx.logger?.info?.(`cardputer-bridge listening on http://${cfg.host}:${cfg.port}`)
      })
    }, 1200)
    return () => {
      clearTimeout(timer)
      try { server.close() } catch {}
    }
  })
}

import { apply } from './plugin/index.js'

const disposers = []
const ctx = {
  logger: { info: console.log, warn: console.warn },
  effect(fn) { disposers.push(fn()) },
  inject(_deps, cb) { cb(ctx) },
  sessionController: {
    async list() { return { items: [{ sessionId: 'test-session', updatedAt: 1, running: false, blank: false, cwd: 'Y:/2026/cardputer' }] } },
    async events() { return { cursor: 2, records: [
      { type: 'event', event: { type: 'user/message', seq: 1, time: 1, data: { content: [{ type: 'text', text: 'hello' }] } } },
      { type: 'event', event: { type: 'assistant/message', seq: 2, time: 2, data: { content: [{ type: 'text', text: 'world' }] } } },
    ] } },
    async state() { return { running: false } },
    async prompt() { return { accepted: true } },
    async create() { return { sessionId: 'new-session' } },
    cancel() { return { accepted: true } },
  },
}

apply(ctx, { host: '127.0.0.1', port: 18765, token: 'test', workspace: 'Y:/2026/cardputer' })
await new Promise(r => setTimeout(r, 1500))
const headers = { 'x-cardputer-token': 'test' }
for (const path of ['/cardputer/health', '/cardputer/sessions', '/cardputer/messages?session=test-session']) {
  const res = await fetch(`http://127.0.0.1:18765${path}`, { headers })
  console.log(path, res.status, await res.text())
}
for (const d of disposers) d()

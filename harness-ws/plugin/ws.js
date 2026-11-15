/**
 * Minimal RFC 6455 WebSocket server side.
 *
 * The Desktop Host ships no `ws` package, so this implements exactly what a
 * LAN-only single-purpose socket needs: the handshake, unmasked server frames,
 * masked client frames, text/binary payloads, fragmentation, and ping/pong.
 * Extensions (permessage-deflate) and subprotocols are not negotiated.
 */
import { createHash } from 'node:crypto'

const WS_GUID = '258EAFA5-E914-47DA-95CA-C5AB0DC85B11'

const OP_CONT = 0x0
const OP_TEXT = 0x1
const OP_BIN = 0x2
const OP_CLOSE = 0x8
const OP_PING = 0x9
const OP_PONG = 0xa

/** Largest message this server accepts or sends; the device sends <=8 KiB frames. */
export const MAX_PAYLOAD = 512 * 1024

export class WebSocketConnection {
  constructor(socket, meta = {}, initial = null) {
    this.socket = socket
    this.meta = meta
    this.buf = Buffer.alloc(0)
    this.fragments = []
    this.fragmentOpcode = 0
    this.fragmentBytes = 0
    this.state = 'open'
    this.onmessage = null
    this.onclose = null
    // Fires for every frame, including protocol-level ping/pong, so the caller
    // can use it as a liveness signal.
    this.onactivity = null

    socket.setNoDelay(true)
    socket.setTimeout(0)
    socket.on('data', chunk => this.#feed(chunk))
    socket.on('error', () => this.destroy())
    socket.on('close', () => this.destroy())

    // Frames that arrived with the handshake must be parsed before new bytes.
    if (initial && initial.length > 0) this.#feed(Buffer.from(initial))
  }

  get open() {
    return this.state === 'open'
  }

  send(text) {
    if (this.state !== 'open') return false
    return this.#write(OP_TEXT, Buffer.from(text, 'utf8'))
  }

  close(code = 1000) {
    if (this.state !== 'open') return
    this.state = 'closing'
    const payload = Buffer.allocUnsafe(2)
    payload.writeUInt16BE(code, 0)
    this.#write(OP_CLOSE, payload)
    try {
      this.socket.end()
    } catch {}
  }

  destroy() {
    if (this.state === 'closed') return
    this.state = 'closed'
    try {
      this.socket.destroy()
    } catch {}
    const notify = this.onclose
    this.onclose = null
    this.onmessage = null
    if (notify) {
      try {
        notify()
      } catch {}
    }
  }

  #write(opcode, payload) {
    const length = payload.length
    let header
    if (length < 126) {
      header = Buffer.allocUnsafe(2)
      header[1] = length
    } else if (length < 65536) {
      header = Buffer.allocUnsafe(4)
      header[1] = 126
      header.writeUInt16BE(length, 2)
    } else {
      header = Buffer.allocUnsafe(10)
      header[1] = 127
      header.writeBigUInt64BE(BigInt(length), 2)
    }
    header[0] = 0x80 | opcode
    try {
      this.socket.write(Buffer.concat([header, payload]))
      return true
    } catch {
      this.destroy()
      return false
    }
  }

  #feed(chunk) {
    if (this.state === 'closed') return
    this.buf = this.buf.length === 0 ? chunk : Buffer.concat([this.buf, chunk])
    try {
      while (this.#parseOne()) {
        if (this.state === 'closed') return
      }
    } catch {
      this.destroy()
    }
  }

  /** Consume one complete frame if buffered; returns false when more bytes are needed. */
  #parseOne() {
    const buf = this.buf
    if (buf.length < 2) return false

    const fin = (buf[0] & 0x80) !== 0
    const opcode = buf[0] & 0x0f
    const masked = (buf[1] & 0x80) !== 0
    let length = buf[1] & 0x7f
    let offset = 2

    if (length === 126) {
      if (buf.length < 4) return false
      length = buf.readUInt16BE(2)
      offset = 4
    } else if (length === 127) {
      if (buf.length < 10) return false
      const wide = buf.readBigUInt64BE(2)
      if (wide > BigInt(MAX_PAYLOAD)) {
        this.close(1009)
        return false
      }
      length = Number(wide)
      offset = 10
    }
    if (length > MAX_PAYLOAD) {
      this.close(1009)
      return false
    }

    // A client frame must be masked (RFC 6455 section 5.1).
    if (!masked) {
      this.close(1002)
      return false
    }
    if (buf.length < offset + 4) return false
    const mask = buf.subarray(offset, offset + 4)
    offset += 4

    if (buf.length < offset + length) return false
    const payload = Buffer.allocUnsafe(length)
    for (let i = 0; i < length; i += 1) payload[i] = buf[offset + i] ^ mask[i & 3]
    this.buf = Buffer.from(buf.subarray(offset + length))

    this.#handleFrame(fin, opcode, payload)
    return true
  }

  #handleFrame(fin, opcode, payload) {
    if (this.onactivity) {
      try {
        this.onactivity()
      } catch {}
    }
    if (opcode === OP_PING) {
      this.#write(OP_PONG, payload)
      return
    }
    if (opcode === OP_PONG) return
    if (opcode === OP_CLOSE) {
      this.close(1000)
      return
    }

    if (opcode === OP_CONT) {
      if (this.fragmentOpcode === 0) {
        this.close(1002)
        return
      }
    } else if (opcode === OP_TEXT || opcode === OP_BIN) {
      if (this.fragmentOpcode !== 0) {
        this.close(1002)
        return
      }
      this.fragmentOpcode = opcode
      this.fragments = []
      this.fragmentBytes = 0
    } else {
      this.close(1002)
      return
    }

    this.fragments.push(payload)
    this.fragmentBytes += payload.length
    if (this.fragmentBytes > MAX_PAYLOAD) {
      this.close(1009)
      return
    }

    if (!fin) return
    const opcodeOfMessage = this.fragmentOpcode
    const body = this.fragments.length === 1 ? this.fragments[0] : Buffer.concat(this.fragments)
    this.fragments = []
    this.fragmentBytes = 0
    this.fragmentOpcode = 0
    if (opcodeOfMessage === OP_TEXT && this.onmessage) {
      try {
        this.onmessage(body.toString('utf8'))
      } catch {}
    }
  }
}

/**
 * Register the WebSocket upgrade on an existing HTTP server.
 *
 * `authorize(request, url)` returns a rejection reason, or null to accept; the
 * caller owns authentication so the handler stays transport-only.
 */
export function attachWebSocket(server, { path, authorize, onConnection }) {
  server.on('upgrade', (request, socket, head) => {
    // This runs on the Host's HTTP server, where an escaping throw would be an
    // uncaught exception in the Desktop process; every branch is contained.
    try {
      handleUpgrade(request, socket, head)
    } catch {
      try {
        socket.destroy()
      } catch {}
    }
  })

  function handleUpgrade(request, socket, head) {
    const reject = status => {
      // A raw rejection keeps the HTTP semantics visible to the client instead
      // of silently closing the TCP stream.
      try {
        socket.write(
          `HTTP/1.1 ${status} ${status === 401 ? 'Unauthorized' : 'Not Found'}\r\n` +
            'connection: close\r\ncontent-length: 0\r\n\r\n',
        )
      } catch {}
      try {
        socket.destroy()
      } catch {}
    }

    let url
    try {
      url = new URL(request.url ?? '/', 'http://cardputer.local')
    } catch {
      reject(404)
      return
    }
    if (url.pathname !== path) {
      reject(404)
      return
    }

    const failure = authorize(request, url)
    if (failure) {
      reject(401)
      return
    }

    const key = request.headers['sec-websocket-key']
    if (typeof key !== 'string' || key.length === 0) {
      reject(404)
      return
    }
    const version = request.headers['sec-websocket-version']
    if (version !== undefined && version !== '13') {
      try {
        socket.write('HTTP/1.1 426 Upgrade Required\r\nsec-websocket-version: 13\r\ncontent-length: 0\r\n\r\n')
      } catch {}
      try {
        socket.destroy()
      } catch {}
      return
    }

    const accept = createHash('sha1').update(key + WS_GUID).digest('base64')
    socket.write(
      'HTTP/1.1 101 Switching Protocols\r\n' +
        'Upgrade: websocket\r\n' +
        'Connection: Upgrade\r\n' +
        `Sec-WebSocket-Accept: ${accept}\r\n\r\n`,
    )
    // `head` may already hold the first frames sent with the handshake.
    const connection = new WebSocketConnection(
      socket,
      { remoteAddress: socket.remoteAddress },
      head,
    )
    onConnection(connection, url)
  }
}

const fs = require("fs");
const http = require("http");
const path = require("path");
const tls = require("tls");

const MQTT_HOST = String(process.env.MQTT_HOST || "0.0.0.0");
const MQTT_PORT = Number(process.env.MQTT_PORT || 8883);
const MQTT_CONTROL_HOST = String(process.env.MQTT_CONTROL_HOST || "127.0.0.1");
const MQTT_CONTROL_PORT = Number(process.env.MQTT_CONTROL_PORT || 4301);
const DATA_DIR = path.join(__dirname, "data");
const CLUSTERS_PATH = path.join(DATA_DIR, "cloud-clusters.json");
const DEVICES_PATH = path.join(DATA_DIR, "cloud-devices.json");
const MAX_PACKET_SIZE = 64 * 1024;
const KEEPALIVE_CHECK_INTERVAL_MS = 1000;
const KEEPALIVE_GRACE_FACTOR = 1.5;

const clients = new Set();
const retained = new Map();
const events = [];

function pushEvent(type, payload = {}) {
  const entry = {
    ts: new Date().toISOString(),
    type,
    payload,
  };
  events.push(entry);
  while (events.length > 500) {
    events.shift();
  }
  console.log(`[mqtt-broker] ${type}`, payload);
  return entry;
}

function readJson(filePath, fallback) {
  try {
    return JSON.parse(fs.readFileSync(filePath, "utf8"));
  } catch (_error) {
    return fallback;
  }
}

function loadRegistry() {
  const clusterRecords = readJson(CLUSTERS_PATH, { clusters: [] }).clusters;
  const deviceRecords = readJson(DEVICES_PATH, { devices: [] }).devices;
  const clusters = new Map();
  const devices = new Map();

  for (const record of Array.isArray(clusterRecords) ? clusterRecords : []) {
    if (record && record.clusterId) {
      clusters.set(String(record.clusterId), record);
    }
  }
  for (const record of Array.isArray(deviceRecords) ? deviceRecords : []) {
    if (record && record.deviceId) {
      devices.set(String(record.deviceId), record);
    }
  }
  return { clusters, devices };
}

function lookupIdentity(identity) {
  const registry = loadRegistry();
  const device = registry.devices.get(String(identity || ""));
  if (!device || device.enabled === false) {
    return null;
  }
  const cluster = registry.clusters.get(String(device.clusterId || ""));
  if (!cluster || cluster.enabled === false || !cluster.bindingSecret) {
    return null;
  }
  return { device, cluster };
}

function encodeRemainingLength(length) {
  const bytes = [];
  let value = length;
  do {
    let encoded = value % 128;
    value = Math.floor(value / 128);
    if (value > 0) {
      encoded |= 0x80;
    }
    bytes.push(encoded);
  } while (value > 0);
  return Buffer.from(bytes);
}

function buildPacket(header, payload = Buffer.alloc(0)) {
  return Buffer.concat([
    Buffer.from([header]),
    encodeRemainingLength(payload.length),
    payload,
  ]);
}

function encodeString(text) {
  const value = Buffer.from(String(text || ""), "utf8");
  const length = Buffer.alloc(2);
  length.writeUInt16BE(value.length, 0);
  return Buffer.concat([length, value]);
}

function parseString(buffer, offset) {
  if (offset + 2 > buffer.length) {
    throw new Error("short_string_length");
  }
  const length = buffer.readUInt16BE(offset);
  const start = offset + 2;
  const end = start + length;
  if (end > buffer.length) {
    throw new Error("short_string_value");
  }
  return {
    value: buffer.subarray(start, end).toString("utf8"),
    offset: end,
  };
}

function extractPacket(state) {
  if (state.buffer.length < 2) {
    return null;
  }

  let multiplier = 1;
  let remainingLength = 0;
  let position = 1;
  let encoded;
  do {
    if (position >= state.buffer.length || position > 4) {
      return null;
    }
    encoded = state.buffer[position++];
    remainingLength += (encoded & 0x7f) * multiplier;
    multiplier *= 128;
  } while ((encoded & 0x80) !== 0);

  if (remainingLength > MAX_PACKET_SIZE) {
    throw new Error("packet_too_large");
  }
  if (state.buffer.length < position + remainingLength) {
    return null;
  }

  const packet = {
    header: state.buffer[0],
    body: state.buffer.subarray(position, position + remainingLength),
  };
  state.buffer = state.buffer.subarray(position + remainingLength);
  return packet;
}

function sendPublish(client, topic, payload, options = {}) {
  const qos = Number(options.qos || 0);
  const retain = Boolean(options.retain);
  const topicBuffer = encodeString(topic);
  const packet = buildPacket(0x30 | (qos << 1) | (retain ? 1 : 0),
    Buffer.concat([topicBuffer, payload]));
  client.socket.write(packet);
}

function publishToSubscribers(topic, payload, options = {}) {
  const retain = Boolean(options.retain);
  const bodyText = payload.toString("utf8").slice(0, 512);
  let bodyJson = null;
  let delivered = 0;

  try {
    bodyJson = JSON.parse(bodyText);
  } catch (_error) {
    bodyJson = null;
  }

  if (retain) {
    if (payload.length === 0) {
      retained.delete(topic);
    } else {
      retained.set(topic, Buffer.from(payload));
    }
  }

  for (const client of clients) {
    if (client.connected && client.subscriptions.has(topic)) {
      sendPublish(client, topic, payload, options);
      delivered++;
    }
  }
  pushEvent("publish", {
    topic,
    bytes: payload.length,
    retain,
    delivered,
    source: options.source || "broker",
    body: bodyText,
    uptime_ms: Number(bodyJson?.uptime_ms || 0),
    unix_time: Number(bodyJson?.unix_time || 0),
  });
  return delivered;
}

function handleConnect(client, body) {
  let cursor = 0;
  const protocol = parseString(body, cursor);
  cursor = protocol.offset;
  if (protocol.value !== "MQTT" || cursor + 4 > body.length) {
    throw new Error("bad_connect_protocol");
  }

  const level = body[cursor++];
  const flags = body[cursor++];
  const keepAlive = body.readUInt16BE(cursor);
  cursor += 2;
  const clientId = parseString(body, cursor);
  cursor = clientId.offset;

  let willTopic = "";
  let willPayload = Buffer.alloc(0);
  if ((flags & 0x04) !== 0) {
    const parsedTopic = parseString(body, cursor);
    cursor = parsedTopic.offset;
    const parsedPayload = parseString(body, cursor);
    cursor = parsedPayload.offset;
    willTopic = parsedTopic.value;
    willPayload = Buffer.from(parsedPayload.value, "utf8");
  }

  let username = "";
  let password = "";
  if ((flags & 0x80) !== 0) {
    const parsed = parseString(body, cursor);
    cursor = parsed.offset;
    username = parsed.value;
  }
  if ((flags & 0x40) !== 0) {
    const parsed = parseString(body, cursor);
    cursor = parsed.offset;
    password = parsed.value;
  }

  const identityRecord = lookupIdentity(client.pskIdentity);
  if (!identityRecord ||
      client.pskIdentity !== identityRecord.device.deviceId ||
      username !== String(identityRecord.device.keyId || "") ||
      password !== String(identityRecord.cluster.bindingSecret || "")) {
    client.socket.write(Buffer.from([0x20, 0x02, 0x00, 0x05]));
    client.socket.end();
    pushEvent("connect-rejected", {
      identity: client.pskIdentity,
      clientId: clientId.value,
    });
    return;
  }

  for (const existing of clients) {
    if (existing !== client &&
        existing.connected &&
        existing.clientId === clientId.value) {
      existing.connected = false;
      existing.graceful = true;
      pushEvent("client-takeover", {
        clientId: clientId.value,
        oldRemote: existing.socket.remoteAddress,
        newRemote: client.socket.remoteAddress,
      });
      existing.socket.destroy();
    }
  }

  client.connected = true;
  client.clientId = clientId.value;
  client.clusterId = identityRecord.device.clusterId;
  client.deviceId = identityRecord.device.deviceId;
  client.keepAlive = keepAlive;
  client.willTopic = willTopic;
  client.willPayload = willPayload;
  client.socket.write(Buffer.from([0x20, 0x02, 0x00, 0x00]));
  pushEvent("connected", {
    identity: client.pskIdentity,
    clientId: client.clientId,
    clusterId: client.clusterId,
    remote: client.socket.remoteAddress,
    cipher: client.socket.getCipher()?.name || "",
  });
}

function handleSubscribe(client, body) {
  if (!client.connected || body.length < 4) {
    throw new Error("bad_subscribe");
  }

  const packetId = body.readUInt16BE(0);
  let cursor = 2;
  const granted = [];
  const retainedMessages = [];
  while (cursor < body.length) {
    const parsed = parseString(body, cursor);
    cursor = parsed.offset;
    if (cursor >= body.length) {
      throw new Error("missing_subscribe_qos");
    }
    const qos = Math.min(body[cursor++], 1);
    client.subscriptions.add(parsed.value);
    granted.push(qos);
    const retainedPayload = retained.get(parsed.value);
    if (retainedPayload) {
      retainedMessages.push({
        topic: parsed.value,
        payload: retainedPayload,
      });
    }
  }

  const response = Buffer.alloc(2 + granted.length);
  response.writeUInt16BE(packetId, 0);
  Buffer.from(granted).copy(response, 2);
  client.socket.write(buildPacket(0x90, response));
  pushEvent("subscribed", {
    clientId: client.clientId,
    topics: Array.from(client.subscriptions),
  });
  for (const message of retainedMessages) {
    sendPublish(client, message.topic, message.payload, { retain: true });
    pushEvent("publish", {
      topic: message.topic,
      bytes: message.payload.length,
      retain: true,
      delivered: 1,
      source: "retained",
      body: message.payload.toString("utf8").slice(0, 512),
    });
  }
  for (const topic of client.subscriptions) {
    if (topic.endsWith("/command/time")) {
      const payload = Buffer.from(JSON.stringify({
        request_id: `broker-time-${Date.now()}`,
        unix_time: Math.floor(Date.now() / 1000),
      }), "utf8");
      sendPublish(client, topic, payload);
      pushEvent("publish", {
        topic,
        bytes: payload.length,
        retain: false,
        delivered: 1,
        source: "server-time",
        body: payload.toString("utf8"),
      });
    }
  }
}

function handlePublish(client, packet) {
  let cursor = 0;
  const parsedTopic = parseString(packet.body, cursor);
  cursor = parsedTopic.offset;
  const qos = (packet.header >> 1) & 0x03;
  let packetId = 0;
  if (qos > 0) {
    if (cursor + 2 > packet.body.length) {
      throw new Error("missing_publish_packet_id");
    }
    packetId = packet.body.readUInt16BE(cursor);
    cursor += 2;
  }
  const payload = packet.body.subarray(cursor);
  publishToSubscribers(parsedTopic.value, payload, {
    qos: 0,
    retain: (packet.header & 0x01) !== 0,
    source: client.clientId,
  });
  if (qos === 1) {
    const ack = Buffer.alloc(2);
    ack.writeUInt16BE(packetId, 0);
    client.socket.write(buildPacket(0x40, ack));
  }
}

function handlePacket(client, packet) {
  const type = packet.header >> 4;
  switch (type) {
    case 1:
      handleConnect(client, packet.body);
      break;
    case 3:
      handlePublish(client, packet);
      break;
    case 8:
      handleSubscribe(client, packet.body);
      break;
    case 12:
      client.socket.write(Buffer.from([0xd0, 0x00]));
      break;
    case 14:
      client.graceful = true;
      client.socket.end();
      break;
    default:
      pushEvent("ignored-packet", {
        clientId: client.clientId,
        type,
      });
      break;
  }
}

const mqttServer = tls.createServer({
  minVersion: "TLSv1.2",
  maxVersion: "TLSv1.2",
  ciphers: [
    "PSK-AES128-GCM-SHA256",
    "PSK-AES256-GCM-SHA384",
    "DHE-PSK-AES128-GCM-SHA256",
  ].join(":"),
  honorCipherOrder: true,
  pskCallback(socket, identity) {
    const record = lookupIdentity(identity);
    if (!record) {
      pushEvent("psk-rejected", {
        identity,
        remote: socket.remoteAddress,
      });
      return null;
    }
    socket._mqttPskIdentity = identity;
    return Buffer.from(record.cluster.bindingSecret, "base64url");
  },
});

mqttServer.on("secureConnection", (socket) => {
  const client = {
    socket,
    pskIdentity: socket._mqttPskIdentity || "",
    clientId: "",
    clusterId: "",
    deviceId: "",
    connected: false,
    graceful: false,
    buffer: Buffer.alloc(0),
    subscriptions: new Set(),
    willTopic: "",
    willPayload: Buffer.alloc(0),
    lastPacketAt: Date.now(),
  };
  clients.add(client);

  socket.on("data", (chunk) => {
    try {
      client.lastPacketAt = Date.now();
      client.buffer = Buffer.concat([client.buffer, chunk]);
      if (client.buffer.length > MAX_PACKET_SIZE) {
        throw new Error("receive_buffer_too_large");
      }
      let packet;
      while ((packet = extractPacket(client)) !== null) {
        handlePacket(client, packet);
      }
    } catch (error) {
      pushEvent("protocol-error", {
        clientId: client.clientId,
        error: error.message,
      });
      socket.destroy();
    }
  });

  socket.on("close", () => {
    clients.delete(client);
    if (!client.graceful && client.willTopic) {
      publishToSubscribers(client.willTopic, client.willPayload, {
        retain: true,
        source: `will:${client.clientId}`,
      });
    }
    pushEvent("disconnected", {
      clientId: client.clientId,
      graceful: client.graceful,
    });
  });

  socket.on("error", (error) => {
    pushEvent("socket-error", {
      clientId: client.clientId,
      error: error.message,
    });
  });
});

mqttServer.on("tlsClientError", (error, socket) => {
  pushEvent("tls-client-error", {
    remote: socket?.remoteAddress || "",
    error: error.message,
  });
});

function sendJson(response, status, body) {
  const payload = Buffer.from(JSON.stringify(body), "utf8");
  response.writeHead(status, {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": payload.length,
  });
  response.end(payload);
}

const controlServer = http.createServer((request, response) => {
  const url = new URL(request.url, `http://${request.headers.host || "localhost"}`);

  if (request.method === "GET" && url.pathname === "/api/summary") {
    return sendJson(response, 200, {
      ok: true,
      mqttHost: MQTT_HOST,
      mqttPort: MQTT_PORT,
      activeClients: Array.from(clients).filter((client) => client.connected).length,
      retainedTopics: retained.size,
      events: events.length,
    });
  }
  if (request.method === "GET" && url.pathname === "/api/clients") {
    return sendJson(response, 200, {
      ok: true,
      clients: Array.from(clients).map((client) => ({
        clientId: client.clientId,
        deviceId: client.deviceId,
        clusterId: client.clusterId,
        connected: client.connected,
        remote: client.socket.remoteAddress,
        subscriptions: Array.from(client.subscriptions),
      })),
    });
  }
  if (request.method === "GET" && url.pathname === "/api/events") {
    return sendJson(response, 200, { ok: true, events });
  }
  if (request.method === "POST" && url.pathname === "/api/publish") {
    const chunks = [];
    request.on("data", (chunk) => chunks.push(chunk));
    request.on("end", () => {
      try {
        const body = JSON.parse(Buffer.concat(chunks).toString("utf8"));
        if (!body.topic || typeof body.payload === "undefined") {
          return sendJson(response, 400, { ok: false, error: "topic_and_payload_required" });
        }
        const payload = Buffer.from(
          typeof body.payload === "string" ? body.payload : JSON.stringify(body.payload),
          "utf8"
        );
        const delivered = publishToSubscribers(String(body.topic), payload, {
          retain: Boolean(body.retain),
          source: "control-api",
        });
        return sendJson(response, 200, { ok: true, delivered });
      } catch (error) {
        return sendJson(response, 400, { ok: false, error: error.message });
      }
    });
    return;
  }

  return sendJson(response, 404, { ok: false, error: "not_found" });
});

mqttServer.listen(MQTT_PORT, MQTT_HOST, () => {
  console.log(`[mqtt-broker] TLS-PSK MQTT listening on ${MQTT_HOST}:${MQTT_PORT}`);
});

controlServer.listen(MQTT_CONTROL_PORT, MQTT_CONTROL_HOST, () => {
  console.log(`[mqtt-broker] control API http://${MQTT_CONTROL_HOST}:${MQTT_CONTROL_PORT}`);
});

const keepAliveTimer = setInterval(() => {
  const now = Date.now();
  for (const client of clients) {
    if (!client.connected || client.keepAlive <= 0) {
      continue;
    }

    const timeoutMs = client.keepAlive * 1000 * KEEPALIVE_GRACE_FACTOR;
    if ((now - client.lastPacketAt) > timeoutMs) {
      client.connected = false;
      pushEvent("keepalive-timeout", {
        clientId: client.clientId,
        idleMs: now - client.lastPacketAt,
        keepAlive: client.keepAlive,
      });
      client.socket.destroy();
    }
  }
}, KEEPALIVE_CHECK_INTERVAL_MS);
keepAliveTimer.unref();

function shutdown() {
  clearInterval(keepAliveTimer);
  for (const client of clients) {
    client.graceful = true;
    client.socket.destroy();
  }
  controlServer.close();
  mqttServer.close();
}

process.on("SIGINT", shutdown);
process.on("SIGTERM", shutdown);

const express = require("express");
const net = require("net");
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const HTTP_PORT = Number(process.env.HTTP_PORT || 4300);
const TCP_PORT = Number(process.env.TCP_PORT || 42001);
const DUPLICATE_MASTER_GRACE_MS = Number(process.env.DUPLICATE_MASTER_GRACE_MS || 15000);
const DUPLICATE_MASTER_SCAN_MS = Number(process.env.DUPLICATE_MASTER_SCAN_MS || 2000);
const CLIENT_STALE_MS = Number(process.env.CLIENT_STALE_MS || 12000);
const REMOTE_ADMIN_PASSWORD = process.env.REMOTE_ADMIN_PASSWORD || "admin";
const DATA_DIR = path.join(__dirname, "data");
const KEYS_PATH = path.join(DATA_DIR, "keys.json");
const LEGACY_BOOTSTRAP_KEY = process.env.BACKEND_KEY || "dev-key";

const DEVICE_STATE_TEXT = {
  0: "停止",
  1: "開啟",
  2: "凍結",
};

const app = express();
app.use(express.json());
app.use((error, _req, res, next) => {
  if (error instanceof SyntaxError && error.status === 400 && "body" in error) {
    pushEvent("http-bad-json", { error: error.message });
    return res.status(400).json({ ok: false, error: "bad_json" });
  }
  return next(error);
});

const clients = new Map();
const events = [];
const duplicateMasterState = new Map();
const knownNodes = new Map();
const sseClients = new Set();
const remoteSessionCache = new Map();
const relayConfirmTimers = new Map();
const relayConfirmInflight = new Set();
const relayConfirmDirty = new Set();
let keyStore = [];
let stateBroadcastScheduled = false;

function ensureDataDir() {
  if (!fs.existsSync(DATA_DIR)) {
    fs.mkdirSync(DATA_DIR, { recursive: true });
  }
}

function saveKeyStore() {
  ensureDataDir();
  fs.writeFileSync(KEYS_PATH, JSON.stringify({ keys: keyStore }, null, 2));
}

function createKeyRecord(label, keyText) {
  return {
    id: crypto.randomUUID(),
    label: (label || "未命名金鑰").trim() || "未命名金鑰",
    key: keyText,
    enabled: true,
    createdAt: new Date().toISOString(),
    lastSeenAt: null,
    lastNodeId: 0,
    lastCluster: "",
    lastDevice: "",
  };
}

function loadKeyStore() {
  ensureDataDir();
  if (fs.existsSync(KEYS_PATH)) {
    try {
      const parsed = JSON.parse(fs.readFileSync(KEYS_PATH, "utf8"));
      keyStore = Array.isArray(parsed.keys) ? parsed.keys : [];
    } catch (_error) {
      keyStore = [];
    }
  }

  if (keyStore.length === 0) {
    keyStore.push(createKeyRecord("bootstrap", LEGACY_BOOTSTRAP_KEY));
    saveKeyStore();
  }
}

function generateKeyText() {
  return crypto.randomBytes(24).toString("base64url");
}

function findKeyRecordBySecret(secret) {
  return keyStore.find((item) => item.key === secret) || null;
}

function findKeyRecordById(id) {
  return keyStore.find((item) => item.id === id) || null;
}

function sanitizeKeyRecord(record) {
  return {
    id: record.id,
    label: record.label,
    key: record.key,
    enabled: record.enabled,
    createdAt: record.createdAt,
    lastSeenAt: record.lastSeenAt,
    lastNodeId: record.lastNodeId,
    lastCluster: record.lastCluster,
    lastDevice: record.lastDevice,
  };
}

function pushEvent(type, payload) {
  const entry = {
    ts: new Date().toISOString(),
    type,
    payload,
  };
  events.push(entry);
  while (events.length > 300) {
    events.shift();
  }
  scheduleStateBroadcast();
  return entry;
}

function deviceStateText(value) {
  return DEVICE_STATE_TEXT[value] || "?芰";
}

function normalizeIp(ipText) {
  const text = String(ipText || "").trim();
  if (text.startsWith("::ffff:")) {
    return text.slice(7);
  }
  return text;
}

function nowIso() {
  return new Date().toISOString();
}

function isClientStale(client) {
  const lastMessageMs = Date.parse(client.lastMessageAt || client.connectedAt);
  if (!Number.isFinite(lastMessageMs)) {
    return false;
  }
  return (Date.now() - lastMessageMs) >= CLIENT_STALE_MS;
}

function clientSummary(client) {
  return {
    id: client.id,
    nodeId: client.nodeId,
    device: client.device,
    cluster: client.cluster,
    ip: client.ip,
    connectedAt: client.connectedAt,
    lastMessageAt: client.lastMessageAt,
    role: client.role,
    relayMask: client.relayMask,
    session: client.session,
    key: client.key,
    stale: isClientStale(client),
    destroyed: !!client.socket.destroyed,
  };
}

function sendJson(client, message) {
  if (!client || !client.socket || client.socket.destroyed || isClientStale(client)) {
    return false;
  }
  return client.socket.write(`${JSON.stringify(message)}\n`);
}

function findClientByNodeId(nodeId) {
  return Array.from(clients.values())
    .filter((item) => Number(item.nodeId) === Number(nodeId) && !item.socket.destroyed && !isClientStale(item))
    .sort((a, b) => Date.parse(b.lastMessageAt) - Date.parse(a.lastMessageAt))[0];
}

function buildTargetedMessage(base, req) {
  const targetNodeId = Number(req.body.targetNodeId || req.body.node_id || 0);
  return targetNodeId > 0 ? { ...base, node_id: targetNodeId } : base;
}

function resolveKnownNode(nodeId) {
  return Array.from(knownNodes.values()).find((item) => Number(item.nodeId) === Number(nodeId)) || null;
}

async function remoteNodeRequest(nodeId, pathText, form) {
  const node = resolveKnownNode(nodeId);
  let token;
  let response;
  let data;

  if (!node || !node.ip || !node.online) {
    throw new Error("node_offline");
  }

  token = await remoteNodeLogin(node);
  response = await fetch(`http://${node.ip}${pathText}`, {
    method: "POST",
    headers: {
      "Content-Type": "application/x-www-form-urlencoded",
      "X-Auth-Token": String(token),
    },
    body: form.toString(),
  });

  data = await response.json();
  if (response.ok && data && data.ok !== false) {
    return data;
  }
  if (data && data.error === "unauthorized") {
    remoteSessionCache.delete(String(Number(node.nodeId || 0)));
    token = await remoteNodeLogin(node);
    response = await fetch(`http://${node.ip}${pathText}`, {
      method: "POST",
      headers: {
        "Content-Type": "application/x-www-form-urlencoded",
        "X-Auth-Token": String(token),
      },
      body: form.toString(),
    });
    return response.json();
  }
  return data;
}

async function remoteNodeGetJson(nodeId, pathText) {
  const node = resolveKnownNode(nodeId);
  let token;
  let response;
  let data;

  if (!node || !node.ip || !node.online) {
    throw new Error("node_offline");
  }

  token = await remoteNodeLogin(node);
  response = await fetch(`http://${node.ip}${pathText}`, {
    headers: {
      "X-Auth-Token": String(token),
    },
  });

  data = await response.json();
  if (response.ok && data && data.ok !== false) {
    return data;
  }
  if (data && data.error === "unauthorized") {
    remoteSessionCache.delete(String(Number(node.nodeId || 0)));
    token = await remoteNodeLogin(node);
    response = await fetch(`http://${node.ip}${pathText}`, {
      headers: {
        "X-Auth-Token": String(token),
      },
    });
    return response.json();
  }
  return data;
}

function isMasterClient(client) {
  return client && (client.role === "master" || Number(client.role) === 2);
}

function knownNodeKey(key, cluster, nodeId) {
  return `${key}|${cluster}|${nodeId}`;
}

function upsertKnownNode(record) {
  const key = knownNodeKey(record.backendKey, record.cluster, record.nodeId);
  const existing = knownNodes.get(key) || {};
  knownNodes.set(key, {
    ...existing,
    ...record,
    lastUpdatedAt: nowIso(),
  });
}

function setKnownNodeRelayMask(nodeId, relayMask) {
  const node = resolveKnownNode(nodeId);
  if (!node) {
    return;
  }

  upsertKnownNode({
    ...node,
    relayMask: Number(relayMask) >>> 0,
    online: 1,
    deviceState: Number(node.deviceState || 0),
  });
  scheduleStateBroadcast();
}

async function remoteNodeLogin(node) {
  const cacheKey = String(Number(node.nodeId || 0));
  const cached = remoteSessionCache.get(cacheKey);
  let loginResponse;
  let loginData;

  if (cached && cached.token && cached.expiresAt > Date.now()) {
    return cached.token;
  }

  loginResponse = await fetch(`http://${node.ip}/api/login`, {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: `password=${encodeURIComponent(REMOTE_ADMIN_PASSWORD)}`,
  });
  loginData = await loginResponse.json();
  if (!loginResponse.ok || !loginData.ok || !loginData.token) {
    throw new Error("remote_login_failed");
  }

  remoteSessionCache.set(cacheKey, {
    token: loginData.token,
    expiresAt: Date.now() + 60000,
  });
  return loginData.token;
}

function predictRelayMask(currentMask, channel, value) {
  const bit = 1 << (Number(channel) - 1);
  if (Number(value) === 0) {
    return (Number(currentMask) >>> 0) & (~bit >>> 0);
  }
  return (Number(currentMask) >>> 0) | bit;
}

function queueRelayStateConfirm(nodeId) {
  const key = String(Number(nodeId));
  const existingTimer = relayConfirmTimers.get(key);

  relayConfirmDirty.add(key);
  if (existingTimer) {
    clearTimeout(existingTimer);
  }
  relayConfirmTimers.set(key, setTimeout(() => {
    relayConfirmTimers.delete(key);
    void runRelayStateConfirm(Number(nodeId));
  }, 250));
}

async function runRelayStateConfirm(nodeId) {
  const key = String(Number(nodeId));

  if (relayConfirmInflight.has(key)) {
    relayConfirmDirty.add(key);
    return;
  }

  relayConfirmInflight.add(key);
  relayConfirmDirty.delete(key);
  try {
    const data = await remoteNodeGetJson(nodeId, "/api/relay");
    if (data && data.ok) {
      setKnownNodeRelayMask(nodeId, Number(data.relay_mask || 0));
    }
  } catch (_error) {
  } finally {
    relayConfirmInflight.delete(key);
    if (relayConfirmDirty.has(key)) {
      relayConfirmDirty.delete(key);
      queueRelayStateConfirm(nodeId);
    }
  }
}

function knownNodeSummary(node) {
  const online = Number(node.online ? 1 : 0);
  const deviceState = online ? Number(node.deviceState || 0) : 0;
  return {
    backendKey: node.backendKey,
    cluster: node.cluster,
    nodeId: node.nodeId,
    device: node.device,
    role: node.role,
    online,
    deviceState,
    deviceStateText: deviceStateText(deviceState),
    relayMask: node.relayMask >>> 0,
    ip: normalizeIp(node.ip),
    lastSeenAt: node.lastSeenAt,
    sourceMasterNodeId: node.sourceMasterNodeId,
  };
}

function buildSummarySnapshot() {
  const clientList = Array.from(clients.values());
  return {
    ok: true,
    httpPort: HTTP_PORT,
    tcpPort: TCP_PORT,
    keyCount: keyStore.length,
    activeClients: clientList.filter((item) => !isClientStale(item)).length,
    rawClients: clientList.length,
    knownNodes: knownNodes.size,
  };
}

function buildClientsSnapshot() {
  return {
    ok: true,
    clients: Array.from(clients.values()).map(clientSummary),
  };
}

function buildKnownNodesSnapshot() {
  return {
    ok: true,
    nodes: Array.from(knownNodes.values())
      .sort((a, b) => {
        if (a.cluster !== b.cluster) {
          return a.cluster.localeCompare(b.cluster);
        }
        return a.nodeId - b.nodeId;
      })
      .map(knownNodeSummary),
  };
}

function buildEventsSnapshot() {
  return {
    ok: true,
    events: events.slice(-40),
  };
}

function buildStatePayload() {
  return {
    summary: buildSummarySnapshot(),
    clients: buildClientsSnapshot(),
    knownNodes: buildKnownNodesSnapshot(),
    events: buildEventsSnapshot(),
  };
}

function broadcastState() {
  const payload = `data: ${JSON.stringify(buildStatePayload())}\n\n`;
  for (const res of Array.from(sseClients.values())) {
    try {
      res.write(payload);
    } catch (_error) {
      sseClients.delete(res);
    }
  }
}

function scheduleStateBroadcast() {
  if (stateBroadcastScheduled) {
    return;
  }
  stateBroadcastScheduled = true;
  setTimeout(() => {
    stateBroadcastScheduled = false;
    broadcastState();
  }, 0);
}

function updateKnownNodeFromClient(client) {
  if (!client.key || !client.cluster || !client.nodeId) {
    return;
  }
  upsertKnownNode({
    backendKey: client.key,
    cluster: client.cluster,
    nodeId: Number(client.nodeId),
    device: client.device || `Node ${client.nodeId}`,
    ip: normalizeIp(client.ip),
    role: Number(client.role) || 0,
    online: isClientStale(client) ? 0 : 1,
    deviceState: isClientStale(client) ? 0 : Number(client.deviceState || 1),
    relayMask: client.relayMask >>> 0,
    lastSeenAt: client.lastMessageAt,
    sourceMasterNodeId: Number(client.nodeId) || 0,
  });
  scheduleStateBroadcast();
}

function markKnownNodeOfflineForClient(client) {
  if (!client || !client.key || !client.cluster || !client.nodeId) {
    return;
  }
  upsertKnownNode({
    backendKey: client.key,
    cluster: client.cluster,
    nodeId: Number(client.nodeId),
    device: client.device || `Node ${client.nodeId}`,
    ip: normalizeIp(client.ip),
    role: Number(client.role) || 0,
    online: 0,
    deviceState: 0,
    relayMask: Number(client.relayMask || 0) >>> 0,
    lastSeenAt: client.lastMessageAt || nowIso(),
    sourceMasterNodeId: Number(client.nodeId) || 0,
  });
  scheduleStateBroadcast();
}

function applyClusterStatusSnapshot(client, message) {
  const groupKey = `${client.key}|${message.cluster || client.cluster || ""}`;
  const seen = new Set();
  const nodes = Array.isArray(message.nodes) ? message.nodes : [];

  for (const node of nodes) {
    const nodeId = Number(node.node_id || 0);
    if (nodeId === 0) {
      continue;
    }
    seen.add(nodeId);
    upsertKnownNode({
      backendKey: client.key,
      cluster: message.cluster || client.cluster || "",
      nodeId,
      device: String(node.name || `Node ${nodeId}`),
      ip: String(node.ip || ""),
      role: Number(node.role || 0),
      online: Number(node.online ? 1 : 0),
      deviceState: Number(node.device_state || 0),
      relayMask: Number(node.relay_mask || 0) >>> 0,
      lastSeenAt: client.lastMessageAt,
      sourceMasterNodeId: Number(message.node_id || client.nodeId || 0),
    });
  }

  for (const [mapKey, entry] of Array.from(knownNodes.entries())) {
    if (`${entry.backendKey}|${entry.cluster}` !== groupKey) {
      continue;
    }
    if (entry.sourceMasterNodeId !== Number(message.node_id || client.nodeId || 0)) {
      continue;
    }
    if (!seen.has(entry.nodeId)) {
      knownNodes.set(mapKey, {
        ...entry,
        online: 0,
        deviceState: 0,
        lastUpdatedAt: nowIso(),
      });
    }
  }
  scheduleStateBroadcast();
}

function scanDuplicateMasters() {
  const groups = new Map();
  const now = Date.now();

  for (const client of clients.values()) {
    if (!client.cluster || !client.key || !isMasterClient(client) || isClientStale(client)) {
      continue;
    }

    const groupKey = `${client.key}|${client.cluster}`;
    const list = groups.get(groupKey) || [];
    list.push(client);
    groups.set(groupKey, list);
  }

  for (const [groupKey, groupClients] of groups.entries()) {
    if (groupClients.length <= 1) {
      duplicateMasterState.delete(groupKey);
      continue;
    }

    const state = duplicateMasterState.get(groupKey) || {
      firstSeenMs: now,
      lastWarnMs: 0,
    };

    if ((now - state.firstSeenMs) >= DUPLICATE_MASTER_GRACE_MS &&
        (state.lastWarnMs === 0 || (now - state.lastWarnMs) >= DUPLICATE_MASTER_GRACE_MS)) {
      pushEvent("duplicate-master-warning", {
        groupKey,
        cluster: groupClients[0].cluster,
        key: groupClients[0].key,
        nodeIds: groupClients.map((item) => item.nodeId),
        devices: groupClients.map((item) => item.device),
      });
      state.lastWarnMs = now;
    }

    duplicateMasterState.set(groupKey, state);
  }

  for (const groupKey of Array.from(duplicateMasterState.keys())) {
    if (!groups.has(groupKey)) {
      duplicateMasterState.delete(groupKey);
    }
  }
}

function scanStaleClients() {
  for (const client of clients.values()) {
    const stale = isClientStale(client);
    if (stale) {
      if (!client.staleWarningIssued) {
        pushEvent("stale-client-warning", {
          id: client.id,
          nodeId: client.nodeId,
          device: client.device,
          cluster: client.cluster,
          key: client.key,
        });
        client.staleWarningIssued = true;
      }
    } else {
      client.staleWarningIssued = false;
    }
    updateKnownNodeFromClient(client);
  }
}

function scanStaleKnownNodes() {
  const nowMs = Date.now();

  for (const node of knownNodes.values()) {
    const lastSeenMs = Date.parse(node.lastSeenAt || node.lastUpdatedAt || 0);
    if (!Number.isFinite(lastSeenMs)) {
      continue;
    }
    if ((nowMs - lastSeenMs) < CLIENT_STALE_MS) {
      continue;
    }
    if (!node.online) {
      continue;
    }
    upsertKnownNode({
      ...node,
      online: 0,
      deviceState: 0,
    });
  }
}

const tcpServer = net.createServer((socket) => {
  const id = `${socket.remoteAddress}:${socket.remotePort}`;
  const client = {
    id,
    socket,
    buffer: "",
    nodeId: null,
    device: null,
    cluster: null,
    ip: socket.remoteAddress,
    role: null,
    relayMask: 0,
    session: 0,
    key: null,
    staleWarningIssued: false,
    connectedAt: nowIso(),
    lastMessageAt: nowIso(),
  };

  clients.set(id, client);
  pushEvent("tcp-connect", { id });

  socket.on("data", (chunk) => {
    client.buffer += chunk.toString("utf8");
    client.lastMessageAt = nowIso();

    while (true) {
      const lineEnd = client.buffer.indexOf("\n");
      if (lineEnd < 0) {
        break;
      }

      const rawLine = client.buffer.slice(0, lineEnd).trim();
      client.buffer = client.buffer.slice(lineEnd + 1);
      if (!rawLine) {
        continue;
      }

      let message;
      try {
        message = JSON.parse(rawLine);
      } catch (_error) {
        pushEvent("parse-error", { id, rawLine });
        continue;
      }

      pushEvent("message", { id, message });

      if (message.type === "hello") {
        const previousClient = findClientByNodeId(message.node_id);
        const keyRecord = findKeyRecordBySecret(message.key);

        client.nodeId = Number(message.node_id) || 0;
        client.device = message.device || "";
        client.cluster = message.cluster || "";
        client.role = message.master ? 2 : 1;
        client.session = Number(message.session || 0);
        client.key = message.key || null;

        if (previousClient && previousClient.id !== client.id) {
          pushEvent("replace-client", {
            oldId: previousClient.id,
            newId: client.id,
            nodeId: message.node_id,
            device: message.device,
          });
          previousClient.socket.destroy();
        }

        if (!keyRecord || !keyRecord.enabled) {
          sendJson(client, { type: "hello_ack", ok: false, error: "bad_key" });
          socket.end();
          continue;
        }

        keyRecord.lastSeenAt = nowIso();
        keyRecord.lastNodeId = client.nodeId;
        keyRecord.lastCluster = client.cluster;
        keyRecord.lastDevice = client.device;
        saveKeyStore();
        updateKnownNodeFromClient(client);
        sendJson(client, { type: "hello_ack", ok: true });
        continue;
      }

      if (message.type === "status") {
        client.nodeId = Number(message.node_id) || client.nodeId;
        client.device = message.device || client.device;
        client.cluster = message.cluster || client.cluster;
        client.role = Number(message.role || client.role || 0);
        client.deviceState = Number(message.device_state || client.deviceState || 1);
        client.relayMask = Number(message.relay_mask || 0) >>> 0;
        updateKnownNodeFromClient(client);
        applyClusterStatusSnapshot(client, message);
        continue;
      }

      if (message.type === "heartbeat") {
        client.nodeId = Number(message.node_id) || client.nodeId;
        client.role = Number(message.role || client.role || 0);
        updateKnownNodeFromClient(client);
        continue;
      }

      if (message.type === "fault_report") {
        console.error("[backend-test] fault-report", JSON.stringify({
          nodeId: client.nodeId,
          device: client.device,
          cluster: client.cluster,
          fault: message,
        }));
        pushEvent("fault-report", {
          nodeId: client.nodeId,
          device: client.device,
          cluster: client.cluster,
          fault: message,
        });
      }
    }
  });

  socket.on("close", () => {
    markKnownNodeOfflineForClient(client);
    clients.delete(id);
    pushEvent("tcp-close", { id });
  });

  socket.on("error", (error) => {
    markKnownNodeOfflineForClient(client);
    pushEvent("tcp-error", { id, error: error.message });
  });
});

function renderHtml() {
  return `<!doctype html>
<html lang="zh-Hant">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>backend-test 控制台</title>
  <style>
    :root { --c1:#41bfd9; --c2:#f15b6c; --bg:#f4f7fb; --card:#ffffff; --text:#24404a; --muted:#6f8790; --line:#d9e7ec; }
    * { box-sizing:border-box; }
    body { margin:0; font-family:Segoe UI, Microsoft JhengHei, sans-serif; background:var(--bg); color:var(--text); }
    header { background:linear-gradient(90deg, var(--c1), #79d7e8); color:#fff; padding:16px 24px; font-size:22px; font-weight:700; }
    main { max-width:1280px; margin:24px auto; padding:0 16px 32px; display:grid; gap:16px; }
    .card { background:var(--card); border:1px solid var(--line); border-radius:14px; box-shadow:0 8px 24px rgba(47,89,105,.08); padding:16px; }
    h2 { margin:0 0 12px; font-size:18px; }
    .row { display:flex; gap:12px; flex-wrap:wrap; align-items:center; }
    input, select, button { font:inherit; }
    input, select { border:1px solid var(--line); border-radius:10px; padding:10px 12px; background:#fff; min-width:160px; }
    button { border:0; border-radius:10px; padding:10px 14px; background:var(--c1); color:#fff; cursor:pointer; }
    button[disabled] { opacity:.55; cursor:not-allowed; }
    button.secondary { background:#eef7fa; color:var(--text); border:1px solid var(--line); }
    button.danger { background:var(--c2); }
    button.pending { background:#c7d1d6; color:#5f6d75; border:1px solid #b5c2c9; }
    table { width:100%; border-collapse:collapse; }
    th, td { text-align:left; padding:10px 8px; border-bottom:1px solid var(--line); vertical-align:top; }
    th { color:var(--muted); font-weight:600; }
    code { background:#eff7fb; padding:2px 6px; border-radius:6px; }
    .muted { color:var(--muted); }
    .pill { display:inline-block; padding:3px 8px; border-radius:999px; background:#eef7fa; font-size:12px; }
    .on { background:#dff7ea; color:#0d7a42; }
    .off { background:#f2f4f6; color:#55626f; }
    pre { margin:0; white-space:pre-wrap; word-break:break-word; background:#0f1720; color:#d6ecf1; padding:12px; border-radius:10px; max-height:360px; overflow:auto; }
  </style>
</head>
<body>
  <header>backend-test 控制台</header>
  <main>
    <section class="card">
      <h2>摘要</h2>
      <div id="summary" class="muted">讀取中...</div>
    </section>
    <section class="card">
      <h2>金鑰管理</h2>
      <div class="row">
        <input id="keyLabel" placeholder="金鑰標籤">
        <button onclick="createKey()">產生新金鑰</button>
      </div>
      <table id="keyTable"></table>
    </section>
    <section class="card">
      <div class="row" style="justify-content:space-between">
        <h2 style="margin:0">目前 TCP 鏈路</h2>
        <button class="secondary" onclick="refresh()">重新整理</button>
      </div>
      <table id="clientTable"></table>
    </section>
    <section class="card">
      <h2>已知裝置</h2>
      <table id="knownNodeTable"></table>
    </section>
    <section class="card">
      <h2>裝置名稱設定</h2>
      <div id="deviceNamePanels" class="muted">沒有可用裝置</div>
    </section>
    <section class="card">
      <h2>Relay 設定</h2>
      <div id="relayConfigPanels" class="muted">沒有可用裝置</div>
    </section>
    <section class="card">
      <h2>手動命令</h2>
      <div class="row">
        <input id="commandNodeId" placeholder="來源節點 nodeId">
        <input id="commandTargetNodeId" placeholder="目標節點 nodeId，可留空">
        <select id="commandType">
          <option value="relay">單路控制</option>
          <option value="mask">遮罩控制</option>
          <option value="all">全部開關</option>
        </select>
        <input id="commandChannel" placeholder="channel">
        <input id="commandValue" placeholder="value / mask">
        <button onclick="sendCommand()">送出</button>
      </div>
      <div id="commandResult" class="muted" style="margin-top:10px;"></div>
    </section>
    <section class="card">
      <h2>事件紀錄</h2>
      <pre id="events">讀取中...</pre>
    </section>
  </main>
  <script>
    async function api(path, options) {
      const response = await fetch(path, options);
      const text = await response.text();
      const data = text ? JSON.parse(text) : {};
      if (!response.ok || data.ok === false) {
        throw new Error(data.error || ('http_' + response.status));
      }
      return data;
    }
    function esc(text) {
      return String(text ?? '')
        .replace(/&/g, '&amp;')
        .replace(/</g, '&lt;')
        .replace(/>/g, '&gt;')
        .replace(/"/g, '&quot;');
    }
    let currentSummary = null;
    let currentClients = [];
    let currentKnownNodes = [];
    let currentEvents = [];
    const deviceNameDirtyMap = {};
    const deviceNameRenderSignatureMap = {};
    const pendingRelayMap = {};
    const pendingRelayBatchMap = {};
    const relayConfigDirtyRows = new Set();
    const relayConfigRenderSignatureMap = {};
    const PENDING_RELAY_TIMEOUT_MS = 3000;
    const RELAY_BATCH_WINDOW_MS = 500;
    const RELAY_STATE_SYNC_INTERVAL_MS = 5000;
    let renderTimer = 0;
    function pendingRelayKey(nodeId, channel) {
      return String(nodeId) + ':' + String(channel);
    }
    function findKnownNodeById(nodeId) {
      return currentKnownNodes.find((item) => Number(item.nodeId) === Number(nodeId)) || null;
    }
    function setMaskBit(mask, channel, value) {
      const bit = 1 << (Number(channel) - 1);
      if (Number(value) === 0) {
        return (Number(mask) >>> 0) & (~bit >>> 0);
      }
      return (Number(mask) >>> 0) | bit;
    }
    function effectiveRelayMask(nodeId, actualMask) {
      let mask = Number(actualMask) >>> 0;
      for (let ch = 1; ch <= 16; ch += 1) {
        const pending = pendingRelayMap[pendingRelayKey(nodeId, ch)];
        if (!pending) {
          continue;
        }
        mask = setMaskBit(mask, ch, pending.value);
      }
      return mask >>> 0;
    }
    function pendingBatchKey(effectiveNodeId) {
      return String(effectiveNodeId);
    }
    function hasPendingRelayWork() {
      return Object.keys(pendingRelayBatchMap).length > 0;
    }
    function clearPendingBatch(batchKey) {
      const batch = pendingRelayBatchMap[batchKey];
      if (!batch) {
        return;
      }
      if (batch.timer) {
        clearTimeout(batch.timer);
      }
      for (const key of batch.pendingKeys) {
        const pending = pendingRelayMap[key];
        if (pending && pending.timer) {
          clearTimeout(pending.timer);
        }
        delete pendingRelayMap[key];
      }
      delete pendingRelayBatchMap[batchKey];
    }
    function reconcilePendingRelays(nodes) {
      for (const batchKey of Object.keys(pendingRelayBatchMap)) {
        const batch = pendingRelayBatchMap[batchKey];
        const node = nodes.find((item) => Number(item.nodeId) === Number(batch.effectiveNodeId));
        if (!node) {
          continue;
        }
        if ((Number(node.relayMask) >>> 0) === (Number(batch.desiredMask) >>> 0)) {
          clearPendingBatch(batchKey);
        }
      }
      for (const item of nodes) {
        for (let ch = 1; ch <= 16; ch += 1) {
          const key = pendingRelayKey(item.nodeId, ch);
          const pending = pendingRelayMap[key];
          if (!pending) {
            continue;
          }
          if (pending.batchKey && pendingRelayBatchMap[pending.batchKey]) {
            continue;
          }
          if (relayOn(item.relayMask, ch) === !!pending.value) {
            if (pending.timer) {
              clearTimeout(pending.timer);
            }
            delete pendingRelayMap[key];
          }
        }
      }
    }
    function pill(text, online) {
      return '<span class="pill ' + (online ? 'on' : 'off') + '">' + text + '</span>';
    }
    function sourceNodeId(item) {
      return Number(item.sourceMasterNodeId || item.nodeId || 0);
    }
    function relayOn(mask, channel) {
      return (((Number(mask) >>> 0) >>> (channel - 1)) & 1) === 1;
    }
    function markRelayButtonPending(buttonEl, channel) {
      if (!buttonEl) {
        return;
      }
      buttonEl.disabled = true;
      buttonEl.className = 'secondary pending';
      buttonEl.textContent = 'CH' + channel + ':等待';
    }
    function scheduleRelayBatchSend(batchKey) {
      const batch = pendingRelayBatchMap[batchKey];
      if (!batch) {
        return;
      }
      if (batch.timer) {
        clearTimeout(batch.timer);
      }
      batch.timer = setTimeout(async () => {
        const activeBatch = pendingRelayBatchMap[batchKey];
        if (!activeBatch) {
          return;
        }
        const body = { mask: Number(activeBatch.desiredMask) >>> 0 };
        if (Number(activeBatch.targetNodeId) > 0) {
          body.targetNodeId = Number(activeBatch.targetNodeId);
        }
        try {
          const result = await api('/send/' + activeBatch.sourceNodeId + '/mask', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(body)
          });
          document.getElementById('commandResult').textContent = JSON.stringify(result);
        } catch (error) {
          for (const key of activeBatch.pendingKeys) {
            const pending = pendingRelayMap[key];
            if (pending && pending.timer) {
              clearTimeout(pending.timer);
            }
            delete pendingRelayMap[key];
          }
          delete pendingRelayBatchMap[batchKey];
          refreshBurst();
          document.getElementById('commandResult').textContent = error.message;
          return;
        }
        delete pendingRelayBatchMap[batchKey];
      }, RELAY_BATCH_WINDOW_MS);
    }
    function renderRelayButtons(item) {
      if (!item.online) {
        return '<span class="muted">離線</span>';
      }
      const sourceId = sourceNodeId(item);
      if (!sourceId) {
        return '<span class="muted">找不到來源 master</span>';
      }
      const targetId = Number(item.nodeId);
      const useTargetId = sourceId === targetId ? 0 : targetId;
      const buttons = [];
      const overlayMask = effectiveRelayMask(item.nodeId, item.relayMask);
      for (let ch = 1; ch <= 16; ch += 1) {
        const pending = pendingRelayMap[pendingRelayKey(item.nodeId, ch)];
        const on = relayOn(overlayMask, ch);
        const buttonClass = pending ? 'secondary pending' : (on ? 'danger' : 'secondary');
        const buttonText = pending ? ('CH' + ch + ':等待') : ('CH' + ch + ':' + (on ? '關' : '開'));
        buttons.push(
          '<button class="' + buttonClass +
          '" style="padding:6px 8px;margin:2px" ' + (pending ? 'disabled ' : '') + 'onclick="quickRelay(this,' +
          sourceId + ',' + useTargetId + ',' + ch + ',' + (on ? 0 : 1) +
          ')">' + buttonText + '</button>'
        );
      }
      return '<div style="display:flex;flex-wrap:wrap;gap:4px">' + buttons.join('') + '</div>';
    }
    function markDeviceNameDirty(nodeId) {
      deviceNameDirtyMap[String(nodeId)] = true;
    }
    function clearDeviceNameDirty(nodeId) {
      delete deviceNameDirtyMap[String(nodeId)];
    }
    function deviceNameNodeSignature(item) {
      return JSON.stringify({
        nodeId: Number(item.nodeId || 0),
        device: String(item.device || ''),
        online: Number(item.online ? 1 : 0),
        role: Number(item.role || 0),
        cluster: String(item.cluster || '')
      });
    }
    function renderDeviceNamePanels(nodes) {
      const container = document.getElementById('deviceNamePanels');
      const existingIds = new Set();
      if (!container) {
        return;
      }
      if (!nodes.length) {
        container.innerHTML = '<div class="muted">沒有可用裝置</div>';
        return;
      }
      for (const item of nodes) {
        const nodeId = String(item.nodeId);
        const panelId = 'device_name_panel_' + nodeId;
        const signature = deviceNameNodeSignature(item);
        let panel = document.getElementById(panelId);
        existingIds.add(panelId);
        if (!panel) {
          panel = document.createElement('div');
          panel.id = panelId;
          container.appendChild(panel);
        }
        if (deviceNameDirtyMap[nodeId]) {
          continue;
        }
        if (deviceNameRenderSignatureMap[nodeId] === signature) {
          continue;
        }
        panel.innerHTML =
          '<div class="row" style="margin-bottom:10px">' +
            '<div style="min-width:240px"><b>' + esc(item.device) + '</b> (' + esc(item.nodeId) + ')</div>' +
            '<input id="dev_name_' + item.nodeId + '" value="' + esc(item.device || '') + '" oninput="markDeviceNameDirty(' + item.nodeId + ')" ' + (item.online ? '' : 'disabled') + '>' +
            '<button class="secondary" ' + (item.online ? '' : 'disabled ') + 'onclick="saveDeviceName(' + item.nodeId + ')">保存名稱</button>' +
            '<span class="muted">' + (item.online ? '在線' : '離線') + '</span>' +
          '</div>';
        deviceNameRenderSignatureMap[nodeId] = signature;
      }
      Array.from(container.children).forEach((child) => {
        if (!existingIds.has(child.id)) {
          container.removeChild(child);
        }
      });
    }
    function relayConfigRowKey(nodeId, channel) {
      return String(nodeId) + ':' + String(channel);
    }
    function markRelayConfigDirty(nodeId, channel) {
      relayConfigDirtyRows.add(relayConfigRowKey(nodeId, channel));
    }
    function clearRelayConfigDirty(nodeId, channel) {
      relayConfigDirtyRows.delete(relayConfigRowKey(nodeId, channel));
    }
    function relayConfigNodeSignature(relays) {
      if (!relays || !Array.isArray(relays.relays)) {
        return 'none';
      }
      return JSON.stringify(relays.relays.map((relay) => ({
        channel: Number(relay.channel || 0),
        name: String(relay.name || ''),
        enabled: Number(relay.enabled ? 1 : 0),
        remember_state: Number(relay.remember_state ? 1 : 0),
        boot_default_on: Number(relay.boot_default_on ? 1 : 0)
      })));
    }
    function buildRelayConfigPanelHtml(item, relays) {
      if (!relays || !Array.isArray(relays.relays)) {
        return '<div style="margin-bottom:14px"><div><b>' + esc(item.device) + '</b> (' + esc(item.nodeId) + ')</div><div class="muted">尚未載入 relay 設定</div></div>';
      }
      const rows = relays.relays.map((relay) =>
        '<tr>' +
          '<td>' + relay.channel + '</td>' +
          '<td><input id="cfg_name_' + item.nodeId + '_' + relay.channel + '" value="' + esc(relay.name || '') + '" oninput="markRelayConfigDirty(' + item.nodeId + ',' + relay.channel + ')"></td>' +
          '<td><input id="cfg_enabled_' + item.nodeId + '_' + relay.channel + '" type="checkbox" ' + (relay.enabled ? 'checked' : '') + ' onchange="markRelayConfigDirty(' + item.nodeId + ',' + relay.channel + ')"></td>' +
          '<td><input id="cfg_remember_' + item.nodeId + '_' + relay.channel + '" type="checkbox" ' + (relay.remember_state ? 'checked' : '') + ' onchange="markRelayConfigDirty(' + item.nodeId + ',' + relay.channel + ')"></td>' +
          '<td><input id="cfg_boot_' + item.nodeId + '_' + relay.channel + '" type="checkbox" ' + (relay.boot_default_on ? 'checked' : '') + ' onchange="markRelayConfigDirty(' + item.nodeId + ',' + relay.channel + ')"></td>' +
          '<td><button class="secondary" onclick="saveRelayConfig(' + item.nodeId + ',' + relay.channel + ')">保存</button></td>' +
        '</tr>'
      ).join('');
      return '<div style="margin-bottom:18px"><div style="font-weight:700;margin-bottom:8px">' + esc(item.device) + ' (' + esc(item.nodeId) + ')</div><table><tr><th>CH</th><th>名稱</th><th>啟用</th><th>記憶</th><th>開機預設開</th><th>操作</th></tr>' + rows + '</table></div>';
    }
    async function loadRelayConfig(nodeId) {
      return api('/api/config/' + nodeId + '/relays');
    }
    function renderRelayConfigPanels(nodes, relayConfigMap) {
      const container = document.getElementById('relayConfigPanels');
      if (!container) {
        return;
      }
      if (!nodes.length) {
        container.innerHTML = '<div class="muted">沒有可用裝置</div>';
        return;
      }
      const existingIds = new Set();
      for (const item of nodes) {
        const nodeId = String(item.nodeId);
        const panelId = 'relay_panel_' + nodeId;
        const relays = relayConfigMap[nodeId];
        const signature = relayConfigNodeSignature(relays);
        const hasDirtyRow = relays && Array.isArray(relays.relays) && relays.relays.some((relay) => relayConfigDirtyRows.has(relayConfigRowKey(item.nodeId, relay.channel)));
        let panel = document.getElementById(panelId);
        existingIds.add(panelId);
        if (!panel) {
          panel = document.createElement('div');
          panel.id = panelId;
          container.appendChild(panel);
        }
        if (hasDirtyRow) {
          continue;
        }
        if (relayConfigRenderSignatureMap[nodeId] === signature) {
          continue;
        }
        panel.innerHTML = buildRelayConfigPanelHtml(item, relays);
        relayConfigRenderSignatureMap[nodeId] = signature;
      }
      Array.from(container.children).forEach((child) => {
        if (!existingIds.has(child.id)) {
          container.removeChild(child);
        }
      });
    }
    const relayConfigCache = {};
    const relayConfigLoadedAt = {};
    function refreshRelayConfigs(nodes, force) {
      const relayTargets = nodes.filter((item) => item.online);
      const pending = relayTargets.filter((item) => {
        const key = String(item.nodeId);
        if (force) {
          return true;
        }
        if (!relayConfigCache[key]) {
          return true;
        }
        return !relayConfigLoadedAt[key] || ((Date.now() - relayConfigLoadedAt[key]) >= 30000);
      });

      if (pending.length === 0) {
        return;
      }

      Promise.allSettled(pending.map(async (item) => {
        const key = String(item.nodeId);
        try {
          relayConfigCache[key] = await loadRelayConfig(item.nodeId);
          relayConfigLoadedAt[key] = Date.now();
        } catch (_error) {
          relayConfigCache[key] = null;
          relayConfigLoadedAt[key] = Date.now();
        }
      })).then(() => {
        renderRelayConfigPanels(nodes, relayConfigCache);
      });
    }
    function flushRenderLiveState() {
      if (!currentSummary) {
        return;
      }

      const summary = currentSummary;
      currentSummary = summary;
      reconcilePendingRelays(currentKnownNodes);

      document.getElementById('summary').innerHTML =
        'HTTP ' + summary.httpPort +
        ' / TCP ' + summary.tcpPort +
        ' / 金鑰 ' + summary.keyCount +
        ' 把 / 活躍鏈路 ' + summary.activeClients +
        ' 條 / 原始 socket ' + summary.rawClients +
        ' 條 / 已知裝置 ' + summary.knownNodes + ' 台';

      document.getElementById('clientTable').innerHTML =
        '<tr><th>nodeId</th><th>裝置</th><th>叢集</th><th>角色</th><th>鏈路狀態</th><th>relayMask</th><th>session</th><th>最後更新</th></tr>' +
        currentClients.map((item) => '<tr>' +
          '<td>' + esc(item.nodeId) + '</td>' +
          '<td>' + esc(item.device) + '</td>' +
          '<td>' + esc(item.cluster) + '</td>' +
          '<td>' + esc(item.role) + '</td>' +
          '<td>' + (item.stale ? pill('stale', false) : pill('active', true)) + '</td>' +
          '<td>' + esc(item.relayMask) + '</td>' +
          '<td>' + esc(item.session) + '</td>' +
          '<td>' + esc(item.lastMessageAt) + '</td>' +
        '</tr>').join('');

      renderDeviceNamePanels(currentKnownNodes);
      if (!hasPendingRelayWork()) {
        document.getElementById('knownNodeTable').innerHTML =
          '<tr><th>nodeId</th><th>裝置</th><th>叢集</th><th>角色</th><th>在線</th><th>狀態</th><th>relayMask</th><th>控制</th><th>最後同步</th></tr>' +
          currentKnownNodes.map((item) => '<tr>' +
            '<td>' + esc(item.nodeId) + '</td>' +
            '<td>' + esc(item.device) + '</td>' +
            '<td>' + esc(item.cluster) + '</td>' +
            '<td>' + esc(item.role) + '</td>' +
            '<td>' + (item.online ? pill('在線', true) : pill('離線', false)) + '</td>' +
            '<td>' + esc(item.deviceStateText) + '</td>' +
            '<td>' + esc(item.relayMask) + '</td>' +
            '<td>' + renderRelayButtons(item) + '</td>' +
            '<td>' + esc(item.lastSeenAt) + '</td>' +
          '</tr>').join('');
      }

      renderRelayConfigPanels(currentKnownNodes, relayConfigCache);
      document.getElementById('events').textContent = JSON.stringify(currentEvents.slice(-40), null, 2);
    }
    function renderLiveState(summary, clients, knownNodes, events) {
      currentSummary = summary;
      currentClients = clients.clients || [];
      currentKnownNodes = knownNodes.nodes || [];
      currentEvents = events.events || [];
      reconcilePendingRelays(currentKnownNodes);
      if (renderTimer) {
        return;
      }
      renderTimer = setTimeout(() => {
        renderTimer = 0;
        flushRenderLiveState();
      }, 16);
    }
    function refreshBurst() {
      if (currentSummary) {
        renderLiveState(currentSummary, { clients: currentClients }, { nodes: currentKnownNodes }, { events: currentEvents });
      }
    }
    function connectStream() {
      const es = new EventSource('/api/stream');
      es.onmessage = (event) => {
        try {
          const payload = JSON.parse(event.data);
          renderLiveState(payload.summary, payload.clients, payload.knownNodes, payload.events);
        } catch (_error) {
        }
      };
      es.onerror = () => {
        es.close();
        setTimeout(connectStream, 1500);
      };
    }
    async function refresh() {
      try {
        const [summary, keys, clients, knownNodes, events] = await Promise.all([
          api('/api/summary'),
          api('/api/keys'),
          api('/clients'),
          api('/api/known-nodes'),
          api('/events')
        ]);

        document.getElementById('summary').innerHTML =
          'HTTP ' + summary.httpPort +
          ' / TCP ' + summary.tcpPort +
          ' / 金鑰 ' + summary.keyCount +
          ' 把 / 活躍鏈路 ' + summary.activeClients +
          ' 條 / 原始 socket ' + summary.rawClients +
          ' 條 / 已知裝置 ' + summary.knownNodes + ' 台';

        document.getElementById('keyTable').innerHTML =
          '<tr><th>標籤</th><th>金鑰</th><th>狀態</th><th>最近使用</th><th>操作</th></tr>' +
          keys.keys.map((item) => '<tr>' +
            '<td>' + esc(item.label) + '</td>' +
            '<td><code>' + esc(item.key) + '</code></td>' +
            '<td>' + (item.enabled ? pill('啟用', true) : pill('停用', false)) + '</td>' +
            '<td>' + esc(item.lastDevice || '-') + ' / ' + esc(item.lastCluster || '-') + '</td>' +
            '<td class="row">' +
              '<button class="secondary" onclick="toggleKey(\\'' + esc(item.id) + '\\',' + (!item.enabled) + ')">' + (item.enabled ? '停用' : '啟用') + '</button>' +
              '<button class="danger" onclick="deleteKey(\\'' + esc(item.id) + '\\')">刪除</button>' +
            '</td>' +
          '</tr>').join('');

        renderLiveState(summary, clients, knownNodes, events);
        refreshRelayConfigs(knownNodes.nodes, false);
        return;
      } catch (error) {
        document.getElementById('summary').textContent = '霈?仃?? ' + error.message;
      }
    }
    async function createKey() {
      const label = document.getElementById('keyLabel').value;
      await api('/api/keys', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ label })
      });
      document.getElementById('keyLabel').value = '';
      refresh();
    }
    async function toggleKey(id, enabled) {
      await api('/api/keys/' + id, {
        method: 'PATCH',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ enabled })
      });
      refresh();
    }
    async function deleteKey(id) {
      await api('/api/keys/' + id, { method: 'DELETE' });
      refresh();
    }
    async function sendCommand() {
      const nodeId = document.getElementById('commandNodeId').value.trim();
      const targetNodeId = document.getElementById('commandTargetNodeId').value.trim();
      const commandType = document.getElementById('commandType').value;
      const channel = document.getElementById('commandChannel').value.trim();
      const value = document.getElementById('commandValue').value.trim();
      const body = {};

      if (targetNodeId) body.targetNodeId = Number(targetNodeId);
      if (commandType === 'relay') {
        body.channel = Number(channel);
        body.value = Number(value);
      } else if (commandType === 'mask') {
        body.mask = Number(value);
      } else {
        body.value = Number(value);
      }

      const result = await api('/send/' + nodeId + '/' + commandType, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      });
      document.getElementById('commandResult').textContent = JSON.stringify(result);
      refreshBurst();
    }
    async function quickRelay(buttonEl, sourceNodeIdValue, targetNodeIdValue, channel, value) {
      const effectiveNodeId = Number(targetNodeIdValue) > 0 ? Number(targetNodeIdValue) : Number(sourceNodeIdValue);
      const key = pendingRelayKey(effectiveNodeId, Number(channel));
      const pendingValue = Number(value);
      const batchKey = pendingBatchKey(effectiveNodeId);
      const node = findKnownNodeById(effectiveNodeId);
      const existingBatch = pendingRelayBatchMap[batchKey];
      const baseMask = existingBatch
        ? Number(existingBatch.desiredMask) >>> 0
        : effectiveRelayMask(effectiveNodeId, Number((node && node.relayMask) || 0) >>> 0);
      const desiredMask = setMaskBit(baseMask, Number(channel), pendingValue);
      pendingRelayMap[key] = {
        value: pendingValue,
        batchKey,
        timer: setTimeout(() => {
          const pending = pendingRelayMap[key];
          if (!pending || pending.value !== pendingValue) {
            return;
          }
          clearPendingBatch(batchKey);
          refreshBurst();
          document.getElementById('commandResult').textContent = 'relay_timeout';
        }, PENDING_RELAY_TIMEOUT_MS)
      };
      markRelayButtonPending(buttonEl, Number(channel));
      const nextBatch = existingBatch || {
        sourceNodeId: Number(sourceNodeIdValue),
        targetNodeId: Number(targetNodeIdValue) > 0 ? Number(targetNodeIdValue) : 0,
        effectiveNodeId,
        desiredMask: baseMask,
        pendingKeys: new Set(),
        timer: 0
      };
      nextBatch.desiredMask = desiredMask;
      nextBatch.pendingKeys.add(key);
      pendingRelayBatchMap[batchKey] = nextBatch;
      scheduleRelayBatchSend(batchKey);
    }
    async function saveRelayConfig(nodeId, channel) {
      const body = {
        channel: Number(channel),
        name: document.getElementById('cfg_name_' + nodeId + '_' + channel).value,
        enabled: document.getElementById('cfg_enabled_' + nodeId + '_' + channel).checked ? 1 : 0,
        remember_state: document.getElementById('cfg_remember_' + nodeId + '_' + channel).checked ? 1 : 0,
        boot_default_on: document.getElementById('cfg_boot_' + nodeId + '_' + channel).checked ? 1 : 0
      };
      const result = await api('/api/config/' + nodeId + '/relay', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body)
      });
      document.getElementById('commandResult').textContent = JSON.stringify(result);
      relayConfigCache[String(nodeId)] = result;
      relayConfigLoadedAt[String(nodeId)] = Date.now();
      clearRelayConfigDirty(nodeId, channel);
      relayConfigRenderSignatureMap[String(nodeId)] = relayConfigNodeSignature(result);
      renderRelayConfigPanels(currentKnownNodes, relayConfigCache);
    }
    async function saveDeviceName(nodeId) {
      const input = document.getElementById('dev_name_' + nodeId);
      const result = await api('/api/config/' + nodeId + '/device-name', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ device_name: input ? input.value : '' })
      });
      document.getElementById('commandResult').textContent = JSON.stringify(result);
      clearDeviceNameDirty(nodeId);
      const node = findKnownNodeById(nodeId);
      if (node) {
        node.device = result.device_name || node.device;
        deviceNameRenderSignatureMap[String(nodeId)] = deviceNameNodeSignature(node);
      }
      renderDeviceNamePanels(currentKnownNodes);
      if (node) {
        renderLiveState(currentSummary, { clients: currentClients }, { nodes: currentKnownNodes }, { events: currentEvents });
      }
      refresh();
    }
    renderDeviceNamePanels([]);
    renderRelayConfigPanels([], {});
    connectStream();
    refresh();
    setInterval(refresh, RELAY_STATE_SYNC_INTERVAL_MS);
  </script>
</body>
</html>`;
}

app.get("/", (_req, res) => {
  res.type("html").send(renderHtml());
});

app.get("/api/stream", (req, res) => {
  res.setHeader("Content-Type", "text/event-stream");
  res.setHeader("Cache-Control", "no-cache, no-transform");
  res.setHeader("Connection", "keep-alive");
  res.flushHeaders?.();
  res.write(`data: ${JSON.stringify(buildStatePayload())}\n\n`);
  sseClients.add(res);

  const keepAlive = setInterval(() => {
    try {
      res.write(": keep-alive\n\n");
    } catch (_error) {
      clearInterval(keepAlive);
      sseClients.delete(res);
    }
  }, 15000);

  req.on("close", () => {
    clearInterval(keepAlive);
    sseClients.delete(res);
  });
});

app.get("/api/summary", (_req, res) => {
  const clientList = Array.from(clients.values());
  res.json({
    ok: true,
    httpPort: HTTP_PORT,
    tcpPort: TCP_PORT,
    keyCount: keyStore.length,
    activeClients: clientList.filter((item) => !isClientStale(item)).length,
    rawClients: clientList.length,
    knownNodes: knownNodes.size,
  });
});

app.get("/api/keys", (_req, res) => {
  res.json({
    ok: true,
    keys: keyStore.map(sanitizeKeyRecord),
  });
});

app.post("/api/keys", (req, res) => {
  const label = typeof req.body.label === "string" ? req.body.label : "";
  const record = createKeyRecord(label, generateKeyText());
  keyStore.push(record);
  saveKeyStore();
  res.json({ ok: true, key: sanitizeKeyRecord(record) });
});

app.patch("/api/keys/:id", (req, res) => {
  const record = findKeyRecordById(req.params.id);
  if (!record) {
    return res.status(404).json({ ok: false, error: "key_not_found" });
  }

  if (typeof req.body.enabled === "boolean") {
    record.enabled = req.body.enabled;
  }
  if (typeof req.body.label === "string" && req.body.label.trim()) {
    record.label = req.body.label.trim();
  }
  saveKeyStore();
  return res.json({ ok: true, key: sanitizeKeyRecord(record) });
});

app.delete("/api/keys/:id", (req, res) => {
  const nextKeys = keyStore.filter((item) => item.id !== req.params.id);
  if (nextKeys.length === keyStore.length) {
    return res.status(404).json({ ok: false, error: "key_not_found" });
  }

  keyStore = nextKeys;
  saveKeyStore();
  return res.json({ ok: true });
});

app.get("/api/known-nodes", (_req, res) => {
  const nodes = Array.from(knownNodes.values())
    .sort((a, b) => {
      if (a.cluster !== b.cluster) {
        return a.cluster.localeCompare(b.cluster);
      }
      return a.nodeId - b.nodeId;
    })
    .map(knownNodeSummary);
  res.json({ ok: true, nodes });
});

app.get("/clients", (_req, res) => {
  res.json({
    ok: true,
    clients: Array.from(clients.values()).map(clientSummary),
  });
});

app.get("/api/clients", (_req, res) => {
  res.json({
    ok: true,
    clients: Array.from(clients.values()).map(clientSummary),
  });
});

app.get("/events", (_req, res) => {
  res.json({
    ok: true,
    events,
  });
});

app.get("/api/config/:nodeId/relays", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const node = resolveKnownNode(nodeId);
  let loginResponse;
  let loginData;
  let token;
  let response;
  let data;

  if (!node || !node.ip || !node.online) {
    return res.status(404).json({ ok: false, error: "node_offline" });
  }

  try {
    loginResponse = await fetch(`http://${node.ip}/api/login`, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: `password=${encodeURIComponent(REMOTE_ADMIN_PASSWORD)}`,
    });
    loginData = await loginResponse.json();
    if (!loginResponse.ok || !loginData.ok || !loginData.token) {
      return res.status(502).json({ ok: false, error: "remote_login_failed" });
    }

    token = loginData.token;
    response = await fetch(`http://${node.ip}/api/config/relays`, {
      headers: { "X-Auth-Token": String(token) },
    });
    data = await response.json();
    if (!response.ok || !data.ok) {
      return res.status(502).json({ ok: false, error: data.error || "remote_fetch_failed" });
    }
    return res.json(data);
  } catch (_error) {
    return res.status(502).json({ ok: false, error: "remote_fetch_failed" });
  }
});

app.post("/api/config/:nodeId/device-name", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const node = resolveKnownNode(nodeId);
  const form = new URLSearchParams();
  let data;

  if (!node || !node.ip || !node.online) {
    return res.status(404).json({ ok: false, error: "node_offline" });
  }
  if (typeof req.body.device_name !== "string" || !req.body.device_name.trim()) {
    return res.status(400).json({ ok: false, error: "bad_device_name" });
  }

  form.set("device_name", req.body.device_name.trim());
  try {
    data = await remoteNodeRequest(nodeId, "/api/config/cluster", form);
    if (!data.ok) {
      return res.status(502).json({ ok: false, error: data.error || "remote_config_failed" });
    }
    upsertKnownNode({
      ...node,
      device: req.body.device_name.trim(),
    });
    return res.json({ ok: true, nodeId, device_name: req.body.device_name.trim() });
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "remote_config_failed" });
  }
});

app.post("/api/config/:nodeId/relay", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const channel = Number(req.body.channel);
  const form = new URLSearchParams();
  let data;

  if (!Number.isInteger(channel) || channel < 1 || channel > 16) {
    return res.status(400).json({ ok: false, error: "bad_channel" });
  }

  form.set("channel", String(channel));
  if (typeof req.body.name === "string") {
    form.set("name", req.body.name);
  }
  if (typeof req.body.enabled !== "undefined") {
    form.set("enabled", Number(req.body.enabled) ? "1" : "0");
  }
  if (typeof req.body.remember_state !== "undefined") {
    form.set("remember_state", Number(req.body.remember_state) ? "1" : "0");
  }
  if (typeof req.body.boot_default_on !== "undefined") {
    form.set("boot_default_on", Number(req.body.boot_default_on) ? "1" : "0");
  }

  try {
    data = await remoteNodeRequest(nodeId, "/api/config/relay", form);
    if (!data.ok) {
      return res.status(502).json({ ok: false, error: data.error || "remote_config_failed" });
    }
    return res.json(data);
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "remote_config_failed" });
  }
});

app.post("/send/:nodeId/relay", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const channel = Number(req.body.channel);
  const value = Number(req.body.value);
  const client = findClientByNodeId(nodeId);

  if (!client) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  if (!Number.isInteger(channel) || channel < 1 || channel > 16 || ![0, 1].includes(value)) {
    return res.status(400).json({ ok: false, error: "bad_relay_request" });
  }

  const message = buildTargetedMessage({ type: "set_relay", channel, value }, req);
  const effectiveNodeId = Number(message.node_id || nodeId);
  if (!sendJson(client, message)) {
    return res.status(409).json({ ok: false, error: "stale_client" });
  }
  if (resolveKnownNode(effectiveNodeId)) {
    queueRelayStateConfirm(effectiveNodeId);
  }
  pushEvent("send-relay", { nodeId, channel, value, targetNodeId: message.node_id || 0 });
  return res.json({ ok: true });
});

app.post("/send/:nodeId/mask", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const mask = Number(req.body.mask);
  const client = findClientByNodeId(nodeId);

  if (!client) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  if (!Number.isInteger(mask) || mask < 0 || mask > 0xffff) {
    return res.status(400).json({ ok: false, error: "bad_mask" });
  }

  const message = buildTargetedMessage({ type: "set_mask", mask }, req);
  const effectiveNodeId = Number(message.node_id || nodeId);
  if (!sendJson(client, message)) {
    return res.status(409).json({ ok: false, error: "stale_client" });
  }
  if (resolveKnownNode(effectiveNodeId)) {
    queueRelayStateConfirm(effectiveNodeId);
  }
  pushEvent("send-mask", { nodeId, mask, targetNodeId: message.node_id || 0 });
  return res.json({ ok: true });
});

app.post("/send/:nodeId/all", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const value = Number(req.body.value);
  const client = findClientByNodeId(nodeId);

  if (!client) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  if (![0, 1].includes(value)) {
    return res.status(400).json({ ok: false, error: "bad_value" });
  }

  const message = buildTargetedMessage({ type: "set_all", value }, req);
  const effectiveNodeId = Number(message.node_id || nodeId);
  if (!sendJson(client, message)) {
    return res.status(409).json({ ok: false, error: "stale_client" });
  }
  if (resolveKnownNode(effectiveNodeId)) {
    queueRelayStateConfirm(effectiveNodeId);
  }
  pushEvent("send-all", { nodeId, value, targetNodeId: message.node_id || 0 });
  return res.json({ ok: true });
});

app.post("/broadcast/ping", (_req, res) => {
  for (const client of clients.values()) {
    sendJson(client, { type: "ping" });
  }
  pushEvent("broadcast-ping", { count: clients.size });
  res.json({ ok: true, count: clients.size });
});

loadKeyStore();

tcpServer.listen(TCP_PORT, () => {
  console.log(`[backend-test] tcp listening on ${TCP_PORT}`);
});

app.listen(HTTP_PORT, () => {
  console.log(`[backend-test] http listening on ${HTTP_PORT}`);
});

setInterval(scanDuplicateMasters, DUPLICATE_MASTER_SCAN_MS);
setInterval(scanStaleClients, DUPLICATE_MASTER_SCAN_MS);
setInterval(scanStaleKnownNodes, DUPLICATE_MASTER_SCAN_MS);

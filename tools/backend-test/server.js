const express = require("express");
const fs = require("fs");
const path = require("path");
const crypto = require("crypto");

const HTTP_PORT = Number(process.env.HTTP_PORT || 4300);
const HTTP_HOST = String(process.env.HTTP_HOST || "127.0.0.1");
const MQTT_CONTROL_PORT = Number(process.env.MQTT_CONTROL_PORT || 4301);
const CLOUD_BROKER_HOST = String(process.env.CLOUD_BROKER_HOST || "mqtt.autocontrol.du1217.club");
const CLOUD_BROKER_PORT = Number(process.env.CLOUD_BROKER_PORT || 8883);
const CLOUD_TRANSPORT = 1; /* MQTT only */
const DUPLICATE_MASTER_GRACE_MS = Number(process.env.DUPLICATE_MASTER_GRACE_MS || 15000);
const DUPLICATE_MASTER_SCAN_MS = Number(process.env.DUPLICATE_MASTER_SCAN_MS || 2000);
const CLIENT_STALE_MS = Number(process.env.CLIENT_STALE_MS || 12000);
const PROVISIONING_RESEND_COOLDOWN_MS = Number(process.env.PROVISIONING_RESEND_COOLDOWN_MS || 30000);
const ALLOW_PRIVATE_KEY_EXPORT = process.env.ALLOW_PRIVATE_KEY_EXPORT === "1";
const REMOTE_CLUSTER_PASSWORD = process.env.REMOTE_CLUSTER_PASSWORD || "autoControl-cluster";
const REMOTE_SESSION_CACHE_TTL_MS = 14 * 60 * 1000;
const DATA_DIR = path.join(__dirname, "data");
const KEYS_PATH = path.join(DATA_DIR, "keys.json");
const CLOUD_DEVICES_PATH = path.join(DATA_DIR, "cloud-devices.json");
const CLOUD_CLUSTERS_PATH = path.join(DATA_DIR, "cloud-clusters.json");
const LEGACY_BOOTSTRAP_KEY = process.env.BACKEND_KEY || "dev-key";
const DASHBOARD_USER = process.env.DASHBOARD_USER || "admin123";
const DASHBOARD_PASSWORD = process.env.DASHBOARD_PASSWORD || "admin123";
const DASHBOARD_SESSION_TTL_MS = 12 * 60 * 60 * 1000;

const DEVICE_STATE_TEXT = {
  0: "線上",
  1: "開啟",
  2: "故障",
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
const cloudDevices = new Map();
const cloudClusters = new Map();
const sseClients = new Set();
const remoteSessionCache = new Map();
const relayConfirmTimers = new Map();
const relayConfirmInflight = new Set();
const relayConfirmDirty = new Set();
const autoProvisionTimers = new Map();
const helloProvisionTimers = new Map();
const dashboardSessions = new Map();
let keyStore = [];
let stateBroadcastScheduled = false;
let cloudRegistrySaveTimer = 0;

/* ?? MQTT command publishing via broker control API (:4301) ??
   The mqtt-psk-broker.js exposes POST /api/publish so the backend
   does not need its own MQTT client ??just HTTP POST to the broker. */
const BROKER_CONTROL_URL = `http://127.0.0.1:${MQTT_CONTROL_PORT}`;

async function mqttPublishCommand(clusterName, topicSuffix, payload) {
  try {
    const topic = `cluster/${clusterName}/command/${topicSuffix}`;
    const payloadJson = JSON.stringify(payload);
    const response = await fetch(`${BROKER_CONTROL_URL}/api/publish`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ topic, payload: payloadJson }),
    });
    const data = await response.json();
    if (data.ok === true) return true;
    console.error(`[mqtt-publish] control API returned: ${JSON.stringify(data)}`);
  } catch (err) {
    console.error(`[mqtt-publish] control API failed: ${err.message}`);
    console.error(`[mqtt-publish] Ensure Mosquitto HTTP listener is on port ${MQTT_CONTROL_PORT}`);
    console.error(`[mqtt-publish] Or set MQTT_CONTROL_PORT env var to the correct port`);
  }
  return false;
}

function ensureDataDir() {
  if (!fs.existsSync(DATA_DIR)) {
    fs.mkdirSync(DATA_DIR, { recursive: true });
  }
}

function parseCookieHeader(header) {
  const out = {};
  for (const part of String(header || "").split(";")) {
    const idx = part.indexOf("=");
    if (idx <= 0) continue;
    const key = part.slice(0, idx).trim();
    const value = part.slice(idx + 1).trim();
    if (key) out[key] = decodeURIComponent(value);
  }
  return out;
}

function createDashboardSession() {
  const token = crypto.randomBytes(24).toString("base64url");
  dashboardSessions.set(token, {
    user: DASHBOARD_USER,
    expiresAt: Date.now() + DASHBOARD_SESSION_TTL_MS,
  });
  return token;
}

function getDashboardSession(req) {
  const cookies = parseCookieHeader(req.headers.cookie || "");
  const token = cookies.ac_backend_session || String(req.headers["x-backend-token"] || "");
  const session = dashboardSessions.get(token);
  if (!session || session.expiresAt < Date.now()) {
    if (token) dashboardSessions.delete(token);
    return null;
  }
  session.expiresAt = Date.now() + DASHBOARD_SESSION_TTL_MS;
  return { token, ...session };
}

function isPublicBackendPath(req) {
  if (req.path === "/api/auth/login" ||
      req.path === "/api/auth/logout" ||
      req.path === "/api/auth/session" ||
      req.path === "/favicon.ico") {
    return true;
  }
  if (req.path === "/api/provision") {
    return true;
  }
  return false;
}

function requireDashboardSession(req, res, next) {
  if (isPublicBackendPath(req)) return next();
  if (req.path === "/" || req.path.startsWith("/api/") || req.path.startsWith("/send/")) {
    const session = getDashboardSession(req);
    if (!session) {
      if (req.path === "/") return next();
      return res.status(401).json({ ok: false, error: "unauthorized" });
    }
    req.dashboardUser = session.user;
  }
  return next();
}

function saveKeyStore() {
  ensureDataDir();
  fs.writeFileSync(KEYS_PATH, JSON.stringify({ keys: keyStore }, null, 2));
}

function saveCloudRegistry() {
  ensureDataDir();
  fs.writeFileSync(CLOUD_DEVICES_PATH, JSON.stringify({
    devices: Array.from(cloudDevices.values()),
  }, null, 2));
  fs.writeFileSync(CLOUD_CLUSTERS_PATH, JSON.stringify({
    clusters: Array.from(cloudClusters.values()),
  }, null, 2));
}

function scheduleCloudRegistrySave() {
  if (cloudRegistrySaveTimer) {
    return;
  }
  cloudRegistrySaveTimer = setTimeout(() => {
    cloudRegistrySaveTimer = 0;
    saveCloudRegistry();
  }, 200);
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
    return;
  }

  if (!keyStore.some((item) => String(item.key || "").trim() === LEGACY_BOOTSTRAP_KEY)) {
    keyStore.unshift(createKeyRecord("bootstrap", LEGACY_BOOTSTRAP_KEY));
    saveKeyStore();
  }
}

function generateKeyText() {
  return crypto.randomBytes(24).toString("base64url");
}

function generateClusterIdentityId() {
  return `cluster-${crypto.randomBytes(6).toString("hex")}`;
}

function generateDeviceIdentityId() {
  return `device-${crypto.randomBytes(6).toString("hex")}`;
}

function generateCloudKeyId() {
  return `key-${crypto.randomBytes(4).toString("hex")}`;
}

function generatePemKeyPair() {
  const { publicKey, privateKey } = crypto.generateKeyPairSync("ed25519");
  return {
    publicPem: publicKey.export({ type: "spki", format: "pem" }).toString(),
    privatePem: privateKey.export({ type: "pkcs8", format: "pem" }).toString(),
  };
}

function fingerprintPem(pem) {
  return crypto.createHash("sha256").update(String(pem || ""), "utf8").digest("hex");
}

function signJson(privatePem, payload) {
  const body = Buffer.from(JSON.stringify(payload), "utf8");
  return crypto.sign(null, body, privatePem).toString("base64");
}

function buildClusterCaBundle(clusterId, keyId) {
  const pair = generatePemKeyPair();
  return {
    caPublicPem: pair.publicPem,
    caPrivatePem: pair.privatePem,
    caFingerprint: fingerprintPem(pair.publicPem),
    bindingSecret: crypto.randomBytes(32).toString("base64url"),
    issuedForClusterId: clusterId,
    issuedByKeyId: keyId,
    issuedAt: nowIso(),
  };
}

function buildDeviceCredentialBundle(clusterRecord, deviceId, deviceName, keyId) {
  const pair = generatePemKeyPair();
  const certificatePayload = {
    version: 1,
    clusterId: clusterRecord.clusterId,
    clusterCaFingerprint: clusterRecord.caFingerprint || "",
    deviceId,
    deviceName,
    keyId,
    publicKeyPem: pair.publicPem,
    issuedAt: nowIso(),
    issuerKeyId: clusterRecord.keyId,
  };
  return {
    devicePublicPem: pair.publicPem,
    devicePrivatePem: pair.privatePem,
    deviceFingerprint: fingerprintPem(pair.publicPem),
    certificatePayload,
    certificateSignature: signJson(clusterRecord.caPrivatePem, certificatePayload),
  };
}

function rotateDeviceCredential(clusterRecord, record) {
  if (!clusterRecord || !record) {
    return null;
  }
  const bundle = buildDeviceCredentialBundle(
    clusterRecord,
    record.deviceId,
    record.device || record.deviceId,
    record.keyId || clusterRecord.keyId || generateCloudKeyId()
  );
  record.devicePublicPem = bundle.devicePublicPem;
  record.devicePrivatePem = bundle.devicePrivatePem;
  record.deviceFingerprint = bundle.deviceFingerprint;
  record.certificatePayload = bundle.certificatePayload;
  record.certificateSignature = bundle.certificateSignature;
  record.lastUpdatedAt = nowIso();
  return record;
}

function rotateClusterCa(clusterRecord) {
  if (!clusterRecord) {
    return null;
  }
  const bundle = buildClusterCaBundle(clusterRecord.clusterId, clusterRecord.keyId || generateCloudKeyId());
  clusterRecord.caPublicPem = bundle.caPublicPem;
  clusterRecord.caPrivatePem = bundle.caPrivatePem;
  clusterRecord.caFingerprint = bundle.caFingerprint;
  clusterRecord.bindingSecret = bundle.bindingSecret;
  clusterRecord.lastUpdatedAt = nowIso();
  for (const deviceRecord of cloudDevices.values()) {
    if (deviceRecord.clusterId === clusterRecord.clusterId) {
      rotateDeviceCredential(clusterRecord, deviceRecord);
      cloudDevices.set(deviceRecord.registryKey, deviceRecord);
    }
  }
  return clusterRecord;
}

function ensureClusterIdentityMaterial(record) {
  if (!record) {
    return null;
  }
  if (record.caPublicPem && record.caPrivatePem && record.caFingerprint && record.bindingSecret) {
    return record;
  }
  const bundle = buildClusterCaBundle(record.clusterId, record.keyId || generateCloudKeyId());
  record.caPublicPem = bundle.caPublicPem;
  record.caPrivatePem = bundle.caPrivatePem;
  record.caFingerprint = bundle.caFingerprint;
  record.bindingSecret = bundle.bindingSecret;
  record.issuedAt = record.issuedAt || bundle.issuedAt;
  saveCloudRegistry();
  return record;
}

function ensureDeviceIdentityMaterial(clusterRecord, record) {
  if (!clusterRecord || !record) {
    return null;
  }
  if (record.devicePublicPem && record.devicePrivatePem && record.deviceFingerprint && record.certificatePayload && record.certificateSignature) {
    return record;
  }
  const bundle = buildDeviceCredentialBundle(
    clusterRecord,
    record.deviceId,
    record.device || record.deviceId,
    record.keyId || clusterRecord.keyId || generateCloudKeyId()
  );
  record.devicePublicPem = bundle.devicePublicPem;
  record.devicePrivatePem = bundle.devicePrivatePem;
  record.deviceFingerprint = bundle.deviceFingerprint;
  record.certificatePayload = bundle.certificatePayload;
  record.certificateSignature = bundle.certificateSignature;
  return record;
}

function sanitizeIdentitySegment(text, fallback) {
  const normalized = String(text || "")
    .trim()
    .replace(/[^A-Za-z0-9._-]+/g, "-")
    .replace(/-+/g, "-")
    .replace(/^-|-$/g, "");
  return normalized || fallback;
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
  const deviceMessage = payload && payload.message && typeof payload.message === "object"
    ? payload.message
    : payload;
  const entry = {
    ts: new Date().toISOString(),
    type,
    payload,
    uptime_ms: Number(deviceMessage?.uptime_ms || 0),
    unix_time: Number(deviceMessage?.unix_time || 0),
  };
  events.push(entry);
  while (events.length > 300) {
    events.shift();
  }
  scheduleStateBroadcast();
  return entry;
}

function deviceStateText(value) {
  return DEVICE_STATE_TEXT[value] || "未知";
}

function normalizeIp(ipText) {
  const text = String(ipText || "").trim();
  if (text.startsWith("::ffff:")) {
    return text.slice(7);
  }
  return text;
}

function cloudClusterKeyOf(record) {
  return String(record?.cloudClusterId || "").trim() || String(record?.cluster || "").trim();
}

function cloudDeviceKeyOf(record) {
  const clusterId = cloudClusterKeyOf(record);
  const deviceId = String(record?.cloudDeviceId || "").trim();
  if (!clusterId || !deviceId) {
    return "";
  }
  return `${clusterId}|${deviceId}`;
}

function findCloudDeviceRecord(clusterId, deviceId) {
  return cloudDevices.get(`${String(clusterId || "").trim()}|${String(deviceId || "").trim()}`) || null;
}

function findCloudClusterRecord(clusterId) {
  return cloudClusters.get(String(clusterId || "").trim()) || null;
}

function buildClusterProvisioning(record) {
  return {
    cloud_enabled: 1,
    cloud_transport: Number(record.transport || CLOUD_TRANSPORT),
    cloud_broker_host: String(record.brokerHost || CLOUD_BROKER_HOST),
    cloud_broker_port: Number(record.brokerPort || CLOUD_BROKER_PORT),
    cloud_cluster_id: record.clusterId,
    cloud_key_id: record.keyId,
    cloud_cluster_ca_fingerprint: record.caFingerprint || "",
    cloud_cluster_binding_secret: record.bindingSecret || "",
  };
}

function buildProvisioningFormFields(provisioning) {
  return Object.entries(provisioning)
    .map(([key, value]) => `${key}=${value}`)
    .join("\n");
}

function buildDeviceProvisioning(clusterRecord, deviceRecord) {
  return {
    ...buildClusterProvisioning(clusterRecord),
    cloud_device_id: deviceRecord.deviceId,
    cloud_key_id: deviceRecord.keyId,
    suggested_device_name: deviceRecord.device || deviceRecord.deviceId,
    cluster_name_hint: clusterRecord.clusterName || clusterRecord.clusterId,
    cloud_device_fingerprint: deviceRecord.deviceFingerprint || "",
  };
}

function buildDeviceProvisioningBundle(clusterRecord, deviceRecord) {
  return {
    cluster: cloudClusterSummary(clusterRecord),
    device: cloudDeviceSummary(deviceRecord),
    provisioning: buildDeviceProvisioning(clusterRecord, deviceRecord),
    trustBundle: {
      clusterCaPublicPem: clusterRecord.caPublicPem || "",
      clusterCaFingerprint: clusterRecord.caFingerprint || "",
      clusterBindingSecret: clusterRecord.bindingSecret || "",
    },
    deviceBundle: {
      devicePublicPem: deviceRecord.devicePublicPem || "",
      devicePrivatePem: ALLOW_PRIVATE_KEY_EXPORT ? (deviceRecord.devicePrivatePem || "") : "",
      deviceFingerprint: deviceRecord.deviceFingerprint || "",
      certificatePayload: deviceRecord.certificatePayload || null,
      certificateSignature: deviceRecord.certificateSignature || "",
    },
    privateKeyExportEnabled: ALLOW_PRIVATE_KEY_EXPORT,
  };
}

function buildProvisioningApplyPayload(clusterRecord, deviceRecord) {
  const provisioning = buildDeviceProvisioning(clusterRecord, deviceRecord);
  const fields = [
    ["cloud_enabled", provisioning.cloud_enabled],
    ["cloud_transport", provisioning.cloud_transport],
    ["cloud_broker_host", provisioning.cloud_broker_host],
    ["cloud_broker_port", provisioning.cloud_broker_port],
    ["cloud_cluster_id", provisioning.cloud_cluster_id],
    ["cloud_device_id", provisioning.cloud_device_id],
    ["cloud_key_id", provisioning.cloud_key_id],
    ["cloud_cluster_ca_fingerprint", provisioning.cloud_cluster_ca_fingerprint || ""],
    ["cloud_cluster_binding_secret", provisioning.cloud_cluster_binding_secret || ""],
  ];
  if (provisioning.suggested_device_name) {
    fields.push(["device_name", provisioning.suggested_device_name]);
  }
  return fields
    .map(([key, value]) => `${encodeURIComponent(String(key))}=${encodeURIComponent(String(value))}`)
    .join("&");
}

function buildProvisioningApplyMessage(clusterRecord, deviceRecord) {
  const provisioning = buildDeviceProvisioning(clusterRecord, deviceRecord);
  return {
    type: "config_cloud",
    cloud_enabled: Number(provisioning.cloud_enabled || 0),
    cloud_transport: Number(provisioning.cloud_transport || 0),
    cloud_broker_host: String(provisioning.cloud_broker_host || ""),
    cloud_broker_port: Number(provisioning.cloud_broker_port || 0),
    cloud_cluster_id: String(provisioning.cloud_cluster_id || ""),
    cloud_device_id: String(provisioning.cloud_device_id || ""),
    cloud_key_id: String(provisioning.cloud_key_id || ""),
    cloud_cluster_ca_fingerprint: String(provisioning.cloud_cluster_ca_fingerprint || ""),
    cloud_cluster_binding_secret: String(provisioning.cloud_cluster_binding_secret || ""),
    device_name: String(provisioning.suggested_device_name || ""),
  };
}

function needsProvisioningForClient(client, clusterRecord, deviceRecord) {
  if (!client || !clusterRecord || !deviceRecord) {
    return false;
  }
  if (String(client.cloudClusterId || "").trim() !== String(clusterRecord.clusterId || "").trim()) {
    return true;
  }
  if (String(client.cloudDeviceId || "").trim() !== String(deviceRecord.deviceId || "").trim()) {
    return true;
  }
  if (String(client.cloudKeyId || "").trim() !== String(deviceRecord.keyId || "").trim()) {
    return true;
  }
  if (!String(client.cloudClusterBindingSecret || "").trim()) {
    return true;
  }
  if (!String(client.cloudClusterCaFingerprint || "").trim()) {
    return true;
  }
  if (!isFingerprintCompatible(clusterRecord.caFingerprint, client.cloudClusterCaFingerprint)) {
    return true;
  }
  return false;
}

function resolveProvisioningRecords(client) {
  const clusterId = cloudClusterKeyOf(client);
  const clusterRecord = findCloudClusterRecord(clusterId);
  const deviceId = String(client?.cloudDeviceId || "").trim() ||
    deriveFallbackDeviceId(client);
  let deviceRecord;

  if (!clusterRecord || !deviceId) {
    return { clusterRecord: null, deviceRecord: null, deviceId: "" };
  }

  deviceRecord = findCloudDeviceRecord(clusterId, deviceId);
  if (!deviceRecord) {
    syncCloudRegistryFromKnownNode({
      nodeId: Number(client?.nodeId || 0),
      device: client?.device || "",
      cluster: client?.cluster || "",
      ip: client?.ip || "",
      role: Number(client?.role || 0),
      online: 1,
      deviceState: Number(client?.deviceState || 0),
      relayMask: Number(client?.relayMask || 0) >>> 0,
      sourceMasterNodeId: Number(client?.nodeId || 0),
      cloudClusterId: clusterId,
      cloudDeviceId: deviceId,
      cloudKeyId: String(client?.cloudKeyId || "").trim(),
      cloudTransport: Number(client?.cloudTransport || 0),
      backendKey: client?.backendKey || "",
      lastSeenAt: nowIso(),
    });
    deviceRecord = findCloudDeviceRecord(clusterId, deviceId);
  }

  return { clusterRecord, deviceRecord, deviceId };
}

function provisioningSignature(clusterRecord, deviceRecord) {
  return JSON.stringify(buildProvisioningApplyMessage(clusterRecord, deviceRecord));
}

function maybeSendProvisioningToClient(client) {
  const { clusterRecord, deviceRecord } = resolveProvisioningRecords(client);
  let signature;
  let lastAppliedMs;
  if (!clusterRecord || !deviceRecord) {
    return false;
  }
  if (!needsProvisioningForClient(client, clusterRecord, deviceRecord)) {
    client.provisioningSignature = "";
    client.provisioningSentAt = 0;
    return false;
  }
  signature = provisioningSignature(clusterRecord, deviceRecord);
  lastAppliedMs = Number(client.provisioningSentAt || 0);
  if ((client.provisioningSignature === signature) &&
      ((Date.now() - lastAppliedMs) < PROVISIONING_RESEND_COOLDOWN_MS)) {
    return false;
  }
  if (!sendJson(client, JSON.parse(signature))) {
    pushEvent("cloud-device-auto-apply-send-failed", {
      nodeId: Number(client?.nodeId || 0),
      clusterId: clusterRecord.clusterId,
      deviceId: deviceRecord.deviceId,
      keyId: String(deviceRecord.keyId || ""),
    });
    return false;
  }
  client.provisioningSignature = signature;
  client.provisioningSentAt = Date.now();
  pushEvent("cloud-device-auto-applied", {
    nodeId: Number(client.nodeId || 0),
    clusterId: clusterRecord.clusterId,
    deviceId: deviceRecord.deviceId,
    keyId: String(deviceRecord.keyId || ""),
    via: "backend_link",
  });
  return true;
}

function scheduleProvisioningForClient(client, delayMs = 6000) {
  void client;
  void delayMs;
}

function isFingerprintCompatible(expected, reported) {
  const expectedText = String(expected || "").trim();
  const reportedText = String(reported || "").trim();
  if (!expectedText || !reportedText) {
    return true;
  }
  if (expectedText === reportedText) {
    return true;
  }
  if (reportedText.length >= 16 && expectedText.startsWith(reportedText)) {
    return true;
  }
  return false;
}

function rejectHello(client, socket, error) {
  pushEvent("hello-reject", {
    nodeId: Number(client?.nodeId || 0),
    device: client?.device || "",
    cluster: client?.cluster || "",
    cloudClusterId: client?.cloudClusterId || "",
    cloudDeviceId: client?.cloudDeviceId || "",
    cloudKeyId: client?.cloudKeyId || "",
    error,
  });
  sendJson(client, { type: "hello_ack", ok: false, error });
  socket.end();
}

async function applyProvisioningToNode(node, clusterRecord, deviceRecord) {
  const nodeId = Number(node.nodeId || 0);
  const activeClient = findClientByNodeId(nodeId);
  let data;

  if (activeClient && sendJson(activeClient, buildProvisioningApplyMessage(clusterRecord, deviceRecord))) {
    return { ok: true, via: "backend_link" };
  }

  data = await remoteNodeRequest(
    nodeId,
    "/api/config/cluster",
    new URLSearchParams(buildProvisioningApplyPayload(clusterRecord, deviceRecord))
  );
  if (!data || data.ok === false) {
    throw new Error(data?.error || "node_http_apply_failed");
  }
  return { ok: true, via: "http" };
}

function nowIso() {
  return new Date().toISOString();
}

function deriveFallbackDeviceId(source) {
  const nodeId = Number(source?.nodeId || 0);
  if (nodeId > 0) {
    return `node-${nodeId}`;
  }
  return sanitizeIdentitySegment(source?.device || "", "node-unknown");
}

function localClusterKey(backendKey, clusterName) {
  return `${String(backendKey || "").trim()}|${String(clusterName || "").trim()}`;
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
    cloudTransport: Number(client.cloudTransport || 0),
    cloudClusterId: client.cloudClusterId || "",
    cloudDeviceId: client.cloudDeviceId || "",
    cloudKeyId: client.cloudKeyId || "",
    ip: client.ip,
    connectedAt: client.connectedAt,
    lastMessageAt: client.lastMessageAt,
    role: client.role,
    term: Number(client.term || 0),
    relayMask: client.relayMask,
    adcValid: Number(client.adcValid || 0),
    adcCtValid: Number(client.adcCtValid || 0),
    adcSignalPresent: Number(client.adcSignalPresent || 0),
    adcRaw: Number(client.adcRaw || 0),
    adcMillivolts: Number(client.adcMillivolts || 0),
    adcMin: Number(client.adcMin || 0),
    adcMax: Number(client.adcMax || 0),
    adcAverage: Number(client.adcAverage || 0),
    adcRawRms: Number(client.adcRawRms || 0),
    adcRmsMillivolts: Number(client.adcRmsMillivolts || 0),
    adcCurrentMilliamps: Number(client.adcCurrentMilliamps || 0),
    adcSampleRateHz: Number(client.adcSampleRateHz || 0),
    adcWindowSamples: Number(client.adcWindowSamples || 0),
    adcSampleCount: Number(client.adcSampleCount || 0),
    adcDmaOverruns: Number(client.adcDmaOverruns || 0),
    adcSaturationCount: Number(client.adcSaturationCount || 0),
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
  const numId = Number(nodeId);
  /* Direct nodeId match */
  let found = Array.from(knownNodes.values()).find((item) => Number(item.nodeId) === numId);
  if (found && numId > 0) return found;
  /* Fallback: match by deviceId substring (e.g., node-XXX-54087705) */
  if (!found) {
    found = Array.from(knownNodes.values()).find((item) =>
      String(item.cloudDeviceId || "").includes(String(nodeId))
    );
  }
  /* Fallback: match by cloudDeviceId exact */
  if (!found) {
    found = Array.from(knownNodes.values()).find((item) =>
      String(item.cloudDeviceId) === String(nodeId)
    );
  }
  return found || null;
}

function effectiveMasterNodeIdForGroup(backendKey, clusterName) {
  const clusterKey = localClusterKey(backendKey, clusterName);
  const activeMasters = Array.from(clients.values())
    .filter((item) =>
      localClusterKey(item.key, item.cluster) === clusterKey &&
      !item.socket.destroyed &&
      !isClientStale(item) &&
      Number(item.role || 0) === 2)
    .sort((a, b) => {
      const termDiff = Number(b.term || 0) - Number(a.term || 0);
      if (termDiff !== 0) {
        return termDiff;
      }
      return Number(a.nodeId || 0) - Number(b.nodeId || 0);
    });
  if (activeMasters.length > 0) {
    return Number(activeMasters[0].nodeId || 0);
  }

  const knownMasters = Array.from(knownNodes.values())
    .filter((item) =>
      localClusterKey(item.backendKey, item.cluster) === clusterKey &&
      Number(item.online ? 1 : 0) === 1 &&
      Number(item.role || 0) === 2)
    .sort((a, b) => {
      const termDiff = Number(b.term || 0) - Number(a.term || 0);
      if (termDiff !== 0) {
        return termDiff;
      }
      return Number(a.nodeId || 0) - Number(b.nodeId || 0);
    });
  if (knownMasters.length > 0) {
    return Number(knownMasters[0].nodeId || 0);
  }

  return 0;
}

async function remoteNodeRequest(nodeId, pathText, method, form) {
  const node = resolveKnownNode(nodeId);
  let token;
  let response;
  let data;
  /* Backward compat: old calls pass (nodeId, path, form) with 3 args */
  if (typeof method !== 'string' || (method !== 'GET' && method !== 'POST' && method !== 'PUT' && method !== 'PATCH' && method !== 'DELETE')) {
    form = method;
    method = 'POST';
  }
  if (!form) form = "";

  if (!node || !node.ip) {
    throw new Error("node_offline");
  }

  token = await remoteNodeLogin(node);
  response = await fetch(`http://${node.ip}${pathText}`, {
    method: method,
    headers: {
      "Content-Type": "application/x-www-form-urlencoded",
      "X-Auth-Token": String(token),
    },
    body: form.toString(),
  });

  data = await readRemoteJson(response);
  if (response.ok && data && data.ok !== false) {
    return data;
  }
  if (data && data.error === "unauthorized") {
    remoteSessionCache.delete(String(Number(node.nodeId || 0)));
    token = await remoteNodeLogin(node);
    response = await fetch(`http://${node.ip}${pathText}`, {
      method: method,
      headers: {
        "Content-Type": "application/x-www-form-urlencoded",
        "X-Auth-Token": String(token),
      },
      body: form.toString(),
    });
    return readRemoteJson(response);
  }
  return data;
}

function loadCloudRegistry() {
  ensureDataDir();
  cloudDevices.clear();
  cloudClusters.clear();
  let dirty = false;

  if (fs.existsSync(CLOUD_CLUSTERS_PATH)) {
    try {
      const parsed = JSON.parse(fs.readFileSync(CLOUD_CLUSTERS_PATH, "utf8"));
      for (const record of Array.isArray(parsed.clusters) ? parsed.clusters : []) {
        if (record && record.clusterId) {
          ensureClusterIdentityMaterial(record);
          cloudClusters.set(record.clusterId, record);
          dirty = true;
        }
      }
    } catch (_error) {
    }
  }

  if (fs.existsSync(CLOUD_DEVICES_PATH)) {
    try {
      const parsed = JSON.parse(fs.readFileSync(CLOUD_DEVICES_PATH, "utf8"));
      for (const record of Array.isArray(parsed.devices) ? parsed.devices : []) {
        if (record && record.registryKey) {
          const clusterRecord = findCloudClusterRecord(record.clusterId);
          if (clusterRecord) {
            ensureDeviceIdentityMaterial(clusterRecord, record);
            dirty = true;
          }
          cloudDevices.set(record.registryKey, record);
        }
      }
    } catch (_error) {
    }
  }

  if (dirty) {
    saveCloudRegistry();
  }

  for (const [registryKey, record] of Array.from(cloudDevices.entries())) {
    if (!record) {
      continue;
    }
    if (String(record.clusterId || "").trim() === "default" &&
        String(record.deviceId || "").trim() === "autoControl" &&
        String(record.device || "").trim() === "autoControl" &&
        !String(record.cloudDeviceId || "").trim()) {
      cloudDevices.delete(registryKey);
    }
  }
}

async function remoteNodeGetJson(nodeId, pathText) {
  const node = resolveKnownNode(nodeId);
  let token;
  let response;
  let data;

  if (!node || !node.ip) {
    throw new Error("node_offline");
  }

  token = await remoteNodeLogin(node);
  response = await fetch(`http://${node.ip}${pathText}`, {
    headers: {
      "X-Auth-Token": String(token),
    },
  });

  data = await readRemoteJson(response);
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
    return readRemoteJson(response);
  }
  return data;
}

async function readRemoteJson(response) {
  const text = await response.text();

  if (!text.trim()) {
    throw new Error(`remote_empty_response_${response.status}`);
  }
  try {
    return JSON.parse(text);
  } catch (_error) {
    throw new Error(`remote_invalid_json_${response.status}`);
  }
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
  const existingSeenMs = Date.parse(existing.lastSeenAt || "");
  const recordSeenMs = Date.parse(record.lastSeenAt || "");

  if (Number.isFinite(existingSeenMs) &&
      Number.isFinite(recordSeenMs) &&
      recordSeenMs < existingSeenMs) {
    return existing;
  }

  const merged = {
      ...existing,
      ...record,
      lastUpdatedAt: nowIso(),
    };
  knownNodes.set(key, merged);
  syncCloudRegistryFromKnownNode(merged);
  return merged;
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

function syncCloudRegistryFromKnownNode(node) {
  const clusterId = cloudClusterKeyOf(node);
  const reportedDeviceId = String(node?.cloudDeviceId || "").trim();
  const deviceId = reportedDeviceId || deriveFallbackDeviceId(node);
  const reportedKeyId = String(node?.cloudKeyId || "").trim();
  const registryKey = clusterId && deviceId ? `${clusterId}|${deviceId}` : "";
  const seenAt = node.lastSeenAt || nowIso();
  const clusterRecord = findCloudClusterRecord(clusterId);
  let existingDevice;
  let deviceRecord;
  let effectiveKeyId;

  if (!clusterId || !deviceId) {
    return;
  }

  existingDevice = cloudDevices.get(registryKey) || {};
  effectiveKeyId = String(reportedKeyId || existingDevice.keyId || "").trim();
  if (!effectiveKeyId && clusterRecord) {
    effectiveKeyId = generateCloudKeyId();
  }
  if (existingDevice.nodeId && Number(existingDevice.nodeId) !== Number(node.nodeId || 0)) {
    pushEvent("cloud-device-rebind", {
      clusterId,
      deviceId,
      previousNodeId: Number(existingDevice.nodeId || 0),
      nextNodeId: Number(node.nodeId || 0),
      previousDeviceName: existingDevice.device || "",
      nextDeviceName: node.device || "",
    });
  }
  deviceRecord = {
    ...existingDevice,
    registryKey,
    clusterId,
    deviceId,
    keyId: effectiveKeyId,
    enabled: existingDevice.enabled !== false,
    transport: Number(node.cloudTransport || 0),
    nodeId: Number(node.nodeId || 0),
    device: node.device || existingDevice.device || "",
    clusterName: node.cluster || existingDevice.clusterName || "",
    ip: normalizeIp(node.ip),
    role: Number(node.role || 0),
    online: Number(node.online ? 1 : 0),
    deviceState: Number(node.online ? (node.deviceState || 0) : 0),
    relayMask: Number(node.relayMask || 0) >>> 0,
    sourceMasterNodeId: Number(node.sourceMasterNodeId || 0),
    backendKey: node.backendKey || existingDevice.backendKey || "",
    firstSeenAt: existingDevice.firstSeenAt || seenAt,
    lastSeenAt: seenAt,
  };

  if (clusterRecord) {
    ensureClusterIdentityMaterial(clusterRecord);
    ensureDeviceIdentityMaterial(clusterRecord, deviceRecord);
    cloudClusters.set(clusterRecord.clusterId, clusterRecord);
    if (!existingDevice.registryKey) {
      pushEvent("cloud-device-auto-enrolled", {
        clusterId,
        deviceId,
        keyId: effectiveKeyId,
        nodeId: Number(node.nodeId || 0),
        device: deviceRecord.device || deviceId,
      });
    }
  }

  cloudDevices.set(registryKey, deviceRecord);

  const clusterDevices = Array.from(cloudDevices.values()).filter((item) => item.clusterId === clusterId);
  const onlineDevices = clusterDevices.filter((item) => Number(item.online) === 1).length;
  const masterDevice = clusterDevices.find((item) => Number(item.role || 0) === 2 && Number(item.online || 0) === 1)
    || clusterDevices.find((item) => Number(item.role || 0) === 2)
    || null;
  const existingCluster = cloudClusters.get(clusterId) || {};

  cloudClusters.set(clusterId, {
    ...existingCluster,
    clusterId,
    keyId: String(existingCluster.keyId || "").trim() || effectiveKeyId,
    enabled: existingCluster.enabled !== false,
    transport: Number(node.cloudTransport || 0),
    brokerHost: existingCluster.brokerHost || CLOUD_BROKER_HOST,
    brokerPort: Number(existingCluster.brokerPort || CLOUD_BROKER_PORT),
    backendKey: node.backendKey || existingCluster.backendKey || "",
    clusterName: node.cluster || existingCluster.clusterName || "",
    lastMasterNodeId: masterDevice ? Number(masterDevice.nodeId || 0) : Number(existingCluster.lastMasterNodeId || 0),
    lastMasterDeviceId: masterDevice ? String(masterDevice.deviceId || "") : String(existingCluster.lastMasterDeviceId || ""),
    onlineDevices,
    knownDevices: clusterDevices.length,
    firstSeenAt: existingCluster.firstSeenAt || seenAt,
    lastSeenAt: seenAt,
  });

  scheduleCloudRegistrySave();

  void clusterRecord;
  void deviceRecord;
}

async function remoteNodeLogin(node) {
  const cacheKey = String(Number(node.nodeId || 0));
  const cached = remoteSessionCache.get(cacheKey);
  let loginResponse;
  let loginData;
  const requesterNodeId = Number(effectiveMasterNodeIdForGroup(node.backendKey, node.cluster) || node.sourceMasterNodeId || node.nodeId || 1);
  const unixTime = Math.floor(Date.now() / 1000);
  const clusterName = String(node.cluster || "default");
  const passwordHash = crypto.createHash("sha256").update(String(REMOTE_CLUSTER_PASSWORD), "utf8").digest();
  const authInput = `${unixTime}|${requesterNodeId}|${clusterName}`;
  const authTag = crypto.createHmac("sha256", passwordHash).update(authInput, "utf8").digest().readUInt32LE(0);

  if (cached && cached.token && cached.expiresAt > Date.now()) {
    return cached.token;
  }

  loginResponse = await fetch(`http://${node.ip}/api/login`, {
    method: "POST",
    headers: { "Content-Type": "application/x-www-form-urlencoded" },
    body: `scope=service`
      + `&node_id=${encodeURIComponent(String(requesterNodeId))}`
      + `&unix_time=${encodeURIComponent(String(unixTime))}`
      + `&auth_tag=${encodeURIComponent(String(authTag))}`,
  });
  loginData = await loginResponse.json();
  if (!loginResponse.ok || !loginData.ok || !loginData.token) {
    throw new Error("remote_login_failed");
  }

  remoteSessionCache.set(cacheKey, {
    token: loginData.token,
    expiresAt: Date.now() + REMOTE_SESSION_CACHE_TTL_MS,
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
  const effectiveMasterNodeId = effectiveMasterNodeIdForGroup(node.backendKey, node.cluster);
  return {
    backendKey: node.backendKey,
    cluster: node.cluster,
    nodeId: node.nodeId,
    device: node.device,
    cloudTransport: Number(node.cloudTransport || 0),
    cloudClusterId: node.cloudClusterId || "",
    cloudDeviceId: node.cloudDeviceId || "",
    cloudKeyId: node.cloudKeyId || "",
    cloudClusterCaFingerprint: node.cloudClusterCaFingerprint || "",
    cloudClusterBindingSecret: node.cloudClusterBindingSecret || "",
    role: node.role,
    term: Number(node.term || 0),
    online,
    deviceState,
    deviceStateText: deviceStateText(deviceState),
    relayMask: node.relayMask >>> 0,
    adcValid: Number(node.adcValid || 0),
    adcCtValid: Number(node.adcCtValid || 0),
    adcSignalPresent: Number(node.adcSignalPresent || 0),
    adcRaw: Number(node.adcRaw || 0),
    adcMillivolts: Number(node.adcMillivolts || 0),
    adcMin: Number(node.adcMin || 0),
    adcMax: Number(node.adcMax || 0),
    adcAverage: Number(node.adcAverage || 0),
    adcRawRms: Number(node.adcRawRms || 0),
    adcRmsMillivolts: Number(node.adcRmsMillivolts || 0),
    adcCurrentMilliamps: Number(node.adcCurrentMilliamps || 0),
    adcSampleRateHz: Number(node.adcSampleRateHz || 0),
    adcWindowSamples: Number(node.adcWindowSamples || 0),
    adcSampleCount: Number(node.adcSampleCount || 0),
    adcDmaOverruns: Number(node.adcDmaOverruns || 0),
    adcSaturationCount: Number(node.adcSaturationCount || 0),
    ip: normalizeIp(node.ip),
    lastSeenAt: node.lastSeenAt,
    sourceMasterNodeId: node.sourceMasterNodeId,
    effectiveMasterNodeId,
  };
}

function cloudDeviceSummary(record) {
  return {
    registryKey: record.registryKey,
    clusterId: record.clusterId,
    deviceId: record.deviceId,
    keyId: record.keyId,
    enabled: record.enabled !== false,
    transport: Number(record.transport || 0),
    nodeId: Number(record.nodeId || 0),
    device: record.device || "",
    clusterName: record.clusterName || "",
    ip: normalizeIp(record.ip),
    role: Number(record.role || 0),
    online: Number(record.online ? 1 : 0),
    deviceState: Number(record.deviceState || 0),
    deviceStateText: deviceStateText(Number(record.deviceState || 0)),
    relayMask: Number(record.relayMask || 0) >>> 0,
    deviceFingerprint: record.deviceFingerprint || "",
    mcuDeviceFingerprint: record.mcuDeviceFingerprint || "",
    deviceFingerprintSource: record.deviceFingerprintSource || "",
    fingerprintBoundAt: record.fingerprintBoundAt || "",
    lastSeenAt: record.lastSeenAt || "",
    firstSeenAt: record.firstSeenAt || "",
    sourceMasterNodeId: Number(record.sourceMasterNodeId || 0),
  };
}

function cloudClusterSummary(record) {
  return {
    clusterId: record.clusterId,
    keyId: record.keyId,
    enabled: record.enabled !== false,
    transport: Number(record.transport || 0),
    brokerHost: record.brokerHost || "",
    brokerPort: Number(record.brokerPort || 0),
    backendKey: record.backendKey || "",
    clusterName: record.clusterName || "",
    caFingerprint: record.caFingerprint || "",
    lastMasterNodeId: Number(record.lastMasterNodeId || 0),
    lastMasterDeviceId: record.lastMasterDeviceId || "",
    lastSeenAt: record.lastSeenAt || "",
    firstSeenAt: record.firstSeenAt || "",
    onlineDevices: Number(record.onlineDevices || 0),
    knownDevices: Number(record.knownDevices || 0),
  };
}

function buildSummarySnapshot() {
  return {
    ok: true,
    httpHost: HTTP_HOST,
    httpPort: HTTP_PORT,
    mqttPort: CLOUD_BROKER_PORT,
    mqttControlPort: MQTT_CONTROL_PORT,
    keyCount: keyStore.length,
    knownNodes: knownNodes.size,
    cloudDevices: cloudDevices.size,
    cloudClusters: cloudClusters.size,
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

function buildCloudDevicesSnapshot() {
  return {
    ok: true,
    devices: Array.from(cloudDevices.values())
      .sort((a, b) => {
        if (a.clusterId !== b.clusterId) {
          return a.clusterId.localeCompare(b.clusterId);
        }
        return a.deviceId.localeCompare(b.deviceId);
      })
      .map(cloudDeviceSummary),
  };
}

function buildCloudClustersSnapshot() {
  return {
    ok: true,
    clusters: Array.from(cloudClusters.values())
      .sort((a, b) => a.clusterId.localeCompare(b.clusterId))
      .map(cloudClusterSummary),
  };
}

function buildStatePayload() {
  return {
    summary: buildSummarySnapshot(),
    clients: buildClientsSnapshot(),
    knownNodes: buildKnownNodesSnapshot(),
    cloudDevices: buildCloudDevicesSnapshot(),
    cloudClusters: buildCloudClustersSnapshot(),
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

/* ?? Sync knownNodes from MQTT broker clients ?? */
let brokerNodesSynced = false;
async function syncKnownNodesFromBroker() {
  try {
    const response = await fetch(`${BROKER_CONTROL_URL}/api/clients`);
    const data = await response.json();
    if (!data.ok || !Array.isArray(data.clients)) return;
    for (const client of data.clients) {
      if (!client.connected || !client.deviceId) continue;
      const registryKey = `${String(client.clusterId || "default")}|${String(client.deviceId)}`;
      const deviceRecord = cloudDevices.get(registryKey);
      const clusterRecord = cloudClusters.get(String(client.clusterId || ""));
      /* Extract nodeId from deviceId if possible (format: node-NNNNNNNNN-NNNNNNNNN) */
      let extractedNodeId = Number(client.nodeId || deviceRecord?.nodeId || 0);
      if (!extractedNodeId && client.deviceId) {
        const m = String(client.deviceId).match(/node-(\d+)-(\d+)/);
        if (m) extractedNodeId = Number(m[2]);
      }
      const knownKey = knownNodeKey(
        clusterRecord?.backendKey || "",
        clusterRecord?.clusterName || String(client.clusterId || "default"),
        extractedNodeId || `dev-${client.deviceId}`
      );
      let node = knownNodes.get(knownKey);
      if (!node) {
        /* Find by deviceId across knownNodes */
        for (const [, n] of knownNodes) {
          if (n.cloudDeviceId === client.deviceId) { node = n; break; }
        }
      }
      if (!node) {
        /* Create new knownNode from broker client info */
        node = {
          backendKey: clusterRecord?.backendKey || "",
          cluster: clusterRecord?.clusterName || String(client.clusterId || "default"),
          nodeId: extractedNodeId,
          cloudClusterId: String(client.clusterId || ""),
          cloudDeviceId: String(client.deviceId),
          cloudKeyId: deviceRecord?.keyId || "",
          online: true,
          device: deviceRecord?.deviceName || client.device || client.clientId || "",
          role: Number(client.role || 0),
          deviceState: Number(client.deviceState || 1),
          deviceStateText: deviceStateText(1),
          relayMask: Number(client.relayMask || 0) >>> 0,
          lastSeen: new Date().toISOString(),
          lastSeenAt: new Date().toISOString(),
          lastSeenMs: Date.now(),
          via: "mqtt-broker",
        };
        const key = node.nodeId > 0
          ? knownNodeKey(node.backendKey, node.cluster, node.nodeId)
          : knownNodeKey(node.backendKey, node.cluster, `dev-${client.deviceId}`);
        knownNodes.set(key, node);
        syncCloudRegistryFromKnownNode(node);
      } else {
        node.online = true;
        node.lastSeen = new Date().toISOString();
        node.lastSeenAt = new Date().toISOString();
        node.lastSeenMs = Date.now();
        if (!node.cloudDeviceId) node.cloudDeviceId = String(client.deviceId);
        if (!node.cloudClusterId) node.cloudClusterId = String(client.clusterId || "");
        if (deviceRecord && deviceRecord.nodeId) node.nodeId = deviceRecord.nodeId;
        if (client.device && !node.device) node.device = String(client.device);
        if (client.role) node.role = Number(client.role || 0);
        if (client.term) node.term = Number(client.term || 0);
        if (typeof client.relayMask !== "undefined") node.relayMask = Number(client.relayMask || 0) >>> 0;
        if (client.lastStatusAt) node.lastSeenAt = String(client.lastStatusAt);
        syncCloudRegistryFromKnownNode(node);
      }
      if (Array.isArray(client.nodes)) {
        for (const remote of client.nodes) {
          const remoteNodeId = Number(remote.node_id || remote.nodeId || 0);
          if (!remoteNodeId) continue;
          upsertKnownNode({
            backendKey: clusterRecord?.backendKey || "",
            cluster: clusterRecord?.clusterName || String(client.clusterId || "default"),
            nodeId: remoteNodeId,
            cloudClusterId: String(remote.cloud_cluster_id || client.clusterId || ""),
            cloudDeviceId: String(remote.cloud_device_id || ""),
            cloudKeyId: String(remote.cloud_key_id || ""),
            online: Number(remote.online ? 1 : 0),
            device: String(remote.name || remote.device || `Node ${remoteNodeId}`),
            role: Number(remote.role || 0),
            term: Number(remote.term || client.term || 0),
            deviceState: Number(remote.device_state || 0),
            relayMask: Number(remote.relay_mask || 0) >>> 0,
            ip: normalizeIp(remote.ip_text || remote.ip || ""),
            lastSeenAt: client.lastStatusAt || new Date().toISOString(),
            lastSeenMs: Date.now(),
            sourceMasterNodeId: extractedNodeId,
            via: "mqtt-broker-status",
          });
        }
      }
    }
    brokerNodesSynced = true;
    scheduleStateBroadcast();
  } catch (_) { /* broker not running yet */ }
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
      cloudTransport: Number(client.cloudTransport || 0),
      cloudClusterId: client.cloudClusterId || "",
      cloudDeviceId: client.cloudDeviceId || "",
      cloudKeyId: client.cloudKeyId || "",
      cloudClusterCaFingerprint: client.cloudClusterCaFingerprint || "",
      cloudClusterBindingSecret: client.cloudClusterBindingSecret || "",
      ip: normalizeIp(client.ip),
    role: Number(client.role) || 0,
    term: Number(client.term || 0),
    online: isClientStale(client) ? 0 : 1,
    deviceState: isClientStale(client) ? 0 : Number(client.deviceState || 1),
    relayMask: client.relayMask >>> 0,
    adcValid: Number(client.adcValid || 0),
    adcCtValid: Number(client.adcCtValid || 0),
    adcSignalPresent: Number(client.adcSignalPresent || 0),
    adcRaw: Number(client.adcRaw || 0),
    adcMillivolts: Number(client.adcMillivolts || 0),
    adcMin: Number(client.adcMin || 0),
    adcMax: Number(client.adcMax || 0),
    adcAverage: Number(client.adcAverage || 0),
    adcRawRms: Number(client.adcRawRms || 0),
    adcRmsMillivolts: Number(client.adcRmsMillivolts || 0),
    adcCurrentMilliamps: Number(client.adcCurrentMilliamps || 0),
    adcSampleRateHz: Number(client.adcSampleRateHz || 0),
    adcWindowSamples: Number(client.adcWindowSamples || 0),
    adcSampleCount: Number(client.adcSampleCount || 0),
    adcDmaOverruns: Number(client.adcDmaOverruns || 0),
    adcSaturationCount: Number(client.adcSaturationCount || 0),
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
      cloudTransport: Number(client.cloudTransport || 0),
      cloudClusterId: client.cloudClusterId || "",
      cloudDeviceId: client.cloudDeviceId || "",
      cloudKeyId: client.cloudKeyId || "",
      ip: normalizeIp(client.ip),
    role: Number(client.role) || 0,
    term: Number(client.term || 0),
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
    const entryKey = knownNodeKey(client.key, message.cluster || client.cluster || "", nodeId);
    const existing = knownNodes.get(entryKey) || {};
    const cloudClusterId = String(node.cloud_cluster_id || message.cloud_cluster_id || client.cloudClusterId || existing.cloudClusterId || "");
    const cloudDeviceId = String(node.cloud_device_id || ((nodeId === Number(message.node_id || client.nodeId || 0)) ? (message.cloud_device_id || client.cloudDeviceId || "") : existing.cloudDeviceId || ""));
    const cloudKeyId = String(node.cloud_key_id || ((nodeId === Number(message.node_id || client.nodeId || 0)) ? (message.cloud_key_id || client.cloudKeyId || "") : existing.cloudKeyId || ""));
    if (nodeId === 0) {
      continue;
    }
    seen.add(nodeId);
      upsertKnownNode({
        backendKey: client.key,
        cluster: message.cluster || client.cluster || "",
        nodeId,
        device: String(node.name || `Node ${nodeId}`),
        cloudTransport: Number(message.cloud_transport || client.cloudTransport || 0),
        cloudClusterId,
        cloudDeviceId,
        cloudKeyId,
        ip: String(node.ip || ""),
      role: Number(node.role || 0),
      term: Number(node.term || message.term || existing.term || 0),
      online: Number(node.online ? 1 : 0),
      deviceState: Number(node.device_state || 0),
      relayMask: Number(node.relay_mask || 0) >>> 0,
      adcValid: Number(node.adc_valid || 0),
      adcCtValid: Number(node.adc_ct_valid || 0),
      adcSignalPresent: Number(node.adc_signal_present || 0),
      adcRaw: Number(node.adc_raw || 0),
      adcMillivolts: Number(node.adc_millivolts || 0),
      adcMin: Number(node.adc_min || 0),
      adcMax: Number(node.adc_max || 0),
      adcAverage: Number(node.adc_average || 0),
      adcRawRms: Number(node.adc_raw_rms || 0),
      adcRmsMillivolts: Number(node.adc_rms_millivolts || 0),
      adcCurrentMilliamps: Number(node.adc_current_milliamps || 0),
      adcSampleRateHz: Number(node.adc_sample_rate_hz || 0),
      adcWindowSamples: Number(node.adc_window_samples || 0),
      adcSampleCount: Number(node.adc_sample_count || 0),
      adcDmaOverruns: Number(node.adc_dma_overruns || 0),
      adcSaturationCount: Number(node.adc_saturation_count || 0),
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
      if (!client.socket.destroyed) {
        client.socket.destroy();
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

/* Raw TCP server (:42001) has been retired ??MQTT is now the only control transport.
   The TCP server code block below is preserved for reference but is dead code.
const _retiredTcpServer = { listen() {} };
if (false) { const tcpServer = net.createServer((socket) => {
  const id = `${socket.remoteAddress}:${socket.remotePort}`;
  const client = {
      id,
      socket,
      buffer: "",
      nodeId: null,
      device: null,
      cluster: null,
      cloudTransport: 0,
      cloudClusterId: "",
      cloudDeviceId: "",
      cloudKeyId: "",
      cloudClusterCaFingerprint: "",
      cloudClusterBindingSecret: "",
      ip: socket.remoteAddress,
    term: 0,
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

      if (message.type === "time_request" && client.key) {
        const unixTime = Math.floor(Date.now() / 1000);
        sendJson(client, {
          type: "time_sync",
          unix_time: unixTime,
          source: "backend",
        });
        pushEvent("time-sync-response", {
          nodeId: Number(client.nodeId || 0),
          unixTime,
        });
        continue;
      }

        if (message.type === "hello") {
          const previousClient = findClientByNodeId(message.node_id);
          const keyRecord = findKeyRecordBySecret(message.key);

          client.nodeId = Number(message.node_id) || 0;
          client.device = message.device || "";
          client.cluster = message.cluster || "";
          client.cloudTransport = Number(message.cloud_transport || 0);
          client.cloudClusterId = message.cloud_cluster_id || "";
          client.cloudDeviceId = message.cloud_device_id || "";
          client.cloudKeyId = message.cloud_key_id || "";
          client.cloudClusterCaFingerprint = message.cloud_cluster_ca_fingerprint || "";
          client.cloudClusterBindingSecret = message.cloud_cluster_binding_secret || "";
          client.term = Number(message.term || client.term || 0);
          client.role = message.master ? 2 : 1;
          client.session = Number(message.session || 0);
          client.key = message.key || null;

          if (!client.cloudClusterId || !client.cloudDeviceId || !client.cloudKeyId) {
            pushEvent("cloud-identity-missing", {
              nodeId: client.nodeId,
              device: client.device,
              cluster: client.cluster,
              cloudClusterId: client.cloudClusterId || "",
              cloudDeviceId: client.cloudDeviceId || "",
              cloudKeyId: client.cloudKeyId || "",
            });
          }

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
            rejectHello(client, socket, "bad_key");
            continue;
          }

          {
            const clusterRecord = findCloudClusterRecord(client.cloudClusterId);
            const deviceRecord = findCloudDeviceRecord(client.cloudClusterId, client.cloudDeviceId);
            if (client.cloudClusterId && !clusterRecord) {
              rejectHello(client, socket, "cloud_cluster_not_found");
              continue;
            }
            if (client.cloudClusterId && client.cloudDeviceId && !deviceRecord) {
              rejectHello(client, socket, "cloud_device_not_found");
              continue;
            }
            if (clusterRecord && clusterRecord.enabled === false) {
              rejectHello(client, socket, "cluster_disabled");
              continue;
            }
            if (clusterRecord && clusterRecord.caFingerprint && client.cloudClusterCaFingerprint &&
                !isFingerprintCompatible(clusterRecord.caFingerprint, client.cloudClusterCaFingerprint)) {
              rejectHello(client, socket, "bad_cloud_cluster_ca_fingerprint");
              continue;
            }
            if (clusterRecord && clusterRecord.bindingSecret && client.cloudClusterBindingSecret && clusterRecord.bindingSecret !== client.cloudClusterBindingSecret) {
              rejectHello(client, socket, "bad_cloud_cluster_binding_secret");
              continue;
            }
            if (clusterRecord && !deviceRecord && clusterRecord.keyId && client.cloudKeyId &&
                clusterRecord.keyId !== client.cloudKeyId) {
              rejectHello(client, socket, "bad_cloud_cluster_key_id");
              continue;
            }
            if (deviceRecord && deviceRecord.enabled === false) {
              rejectHello(client, socket, "device_disabled");
              continue;
            }
            if (deviceRecord && deviceRecord.keyId && client.cloudKeyId && deviceRecord.keyId !== client.cloudKeyId) {
              rejectHello(client, socket, "bad_cloud_device_key_id");
              continue;
            }
          }

          keyRecord.lastSeenAt = nowIso();
        keyRecord.lastNodeId = client.nodeId;
        keyRecord.lastCluster = client.cluster;
        keyRecord.lastDevice = client.device;
        saveKeyStore();
        pushEvent("hello-ok", {
          nodeId: client.nodeId,
          device: client.device,
          cluster: client.cluster,
          cloudClusterId: client.cloudClusterId || "",
          cloudDeviceId: client.cloudDeviceId || "",
          cloudKeyId: client.cloudKeyId || "",
        });
        updateKnownNodeFromClient(client);
        scheduleProvisioningForClient(client);
        continue;
      }

        if (message.type === "status") {
          client.nodeId = Number(message.node_id) || client.nodeId;
          client.device = message.device || client.device;
          client.cluster = message.cluster || client.cluster;
          client.cloudTransport = Number(message.cloud_transport || client.cloudTransport || 0);
          client.cloudClusterId = message.cloud_cluster_id || client.cloudClusterId || "";
          client.cloudDeviceId = message.cloud_device_id || client.cloudDeviceId || "";
          client.cloudKeyId = message.cloud_key_id || client.cloudKeyId || "";
          client.cloudClusterCaFingerprint = message.cloud_cluster_ca_fingerprint || client.cloudClusterCaFingerprint || "";
          client.term = Number(message.term || client.term || 0);
          client.role = Number(message.role || client.role || 0);
        client.deviceState = Number(message.device_state || client.deviceState || 1);
        client.relayMask = Number(message.relay_mask || 0) >>> 0;
        client.adcValid = Number(message.adc_valid || 0);
        client.adcCtValid = Number(message.adc_ct_valid || 0);
        client.adcSignalPresent = Number(message.adc_signal_present || 0);
        client.adcRaw = Number(message.adc_raw || 0);
        client.adcMillivolts = Number(message.adc_millivolts || 0);
        client.adcMin = Number(message.adc_min || 0);
        client.adcMax = Number(message.adc_max || 0);
        client.adcAverage = Number(message.adc_average || 0);
        client.adcRawRms = Number(message.adc_raw_rms || 0);
        client.adcRmsMillivolts = Number(message.adc_rms_millivolts || 0);
        client.adcCurrentMilliamps = Number(message.adc_current_milliamps || 0);
        client.adcSampleRateHz = Number(message.adc_sample_rate_hz || 0);
        client.adcWindowSamples = Number(message.adc_window_samples || 0);
        client.adcSampleCount = Number(message.adc_sample_count || 0);
        client.adcDmaOverruns = Number(message.adc_dma_overruns || 0);
        client.adcSaturationCount = Number(message.adc_saturation_count || 0);
        updateKnownNodeFromClient(client);
        applyClusterStatusSnapshot(client, message);
        continue;
      }

      if (message.type === "heartbeat") {
        client.nodeId = Number(message.node_id) || client.nodeId;
        client.term = Number(message.term || client.term || 0);
        client.role = Number(message.role || client.role || 0);
        updateKnownNodeFromClient(client);
        continue;
      }

        if (message.type === "fault_report") {
          console.error("[backend-test] fault-report", JSON.stringify({
            nodeId: client.nodeId,
            device: client.device,
            cluster: client.cluster,
            cloudClusterId: client.cloudClusterId || "",
            cloudDeviceId: client.cloudDeviceId || "",
            cloudKeyId: client.cloudKeyId || "",
            fault: message,
          }));
          pushEvent("fault-report", {
            nodeId: client.nodeId,
            device: client.device,
            cluster: client.cluster,
            cloudClusterId: client.cloudClusterId || "",
            cloudDeviceId: client.cloudDeviceId || "",
            cloudKeyId: client.cloudKeyId || "",
            fault: message,
          });
        }
    }
  });

  socket.on("close", () => {
    helloProvisionTimers.delete(String(Number(client.nodeId || 0)));
    markKnownNodeOfflineForClient(client);
    clients.delete(id);
    pushEvent("tcp-close", { id });
  });

  socket.on("error", (error) => {
    helloProvisionTimers.delete(String(Number(client.nodeId || 0)));
    markKnownNodeOfflineForClient(client);
    pushEvent("tcp-error", { id, error: error.message });
  });
}); } /* end of retired TCP server block */

const DASHBOARD_PATH = path.join(__dirname, "dashboard.html");
let _dashboardHtml = null;
let _dashboardMtime = 0;

function renderHtml() {
  try {
    const stat = fs.statSync(DASHBOARD_PATH);
    if (!_dashboardHtml || stat.mtimeMs > _dashboardMtime) {
      _dashboardHtml = fs.readFileSync(DASHBOARD_PATH, "utf8");
      _dashboardMtime = stat.mtimeMs;
    }
    return _dashboardHtml;
  } catch (_error) {
    return "<!doctype html><html lang=zh-Hant><head><meta charset=utf-8><title>autoControl</title></head><body><h1>Dashboard not found</h1><p>Place dashboard.html in tools/backend-test/</p></body></html>";
  }
}

app.post("/api/auth/login", (req, res) => {
  const username = String(req.body.username || "").trim();
  const password = String(req.body.password || "");
  if (username !== DASHBOARD_USER || password !== DASHBOARD_PASSWORD) {
    pushEvent("dashboard-login-failed", { username });
    return res.status(401).json({ ok: false, error: "bad_credentials" });
  }
  const token = createDashboardSession();
  res.setHeader(
    "Set-Cookie",
    `ac_backend_session=${encodeURIComponent(token)}; Path=/; HttpOnly; SameSite=Lax; Max-Age=${Math.floor(DASHBOARD_SESSION_TTL_MS / 1000)}`
  );
  pushEvent("dashboard-login", { username });
  return res.json({ ok: true, user: username, expires_in_ms: DASHBOARD_SESSION_TTL_MS });
});

app.post("/api/auth/logout", (req, res) => {
  const session = getDashboardSession(req);
  if (session && session.token) dashboardSessions.delete(session.token);
  res.setHeader("Set-Cookie", "ac_backend_session=; Path=/; HttpOnly; SameSite=Lax; Max-Age=0");
  return res.json({ ok: true });
});

app.get("/api/auth/session", (req, res) => {
  const session = getDashboardSession(req);
  if (!session) return res.status(401).json({ ok: false, error: "unauthorized" });
  return res.json({ ok: true, user: session.user, expires_at: session.expiresAt });
});

app.use(requireDashboardSession);

app.get("/", (_req, res) => {
  res.type("html").send(renderHtml());
});

app.get("/favicon.ico", (_req, res) => {
  res.status(204).end();
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
  res.json(buildSummarySnapshot());
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

app.get("/api/known-nodes", async (_req, res) => {
  await syncKnownNodesFromBroker();
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

app.get("/api/cloud/devices", async (_req, res) => {
  await syncKnownNodesFromBroker();
  res.json(buildCloudDevicesSnapshot());
});

app.get("/api/cloud/clusters", async (_req, res) => {
  await syncKnownNodesFromBroker();
  res.json(buildCloudClustersSnapshot());
});

app.post("/api/cloud/clusters", (req, res) => {
  const clusterId = String(req.body.clusterId || generateClusterIdentityId()).trim();
  const clusterName = String(req.body.clusterName || clusterId).trim();
  const keyId = String(req.body.keyId || generateCloudKeyId()).trim();
  const brokerHost = String(req.body.brokerHost || CLOUD_BROKER_HOST).trim();
  const brokerPort = Number(req.body.brokerPort || CLOUD_BROKER_PORT);
  const caBundle = buildClusterCaBundle(clusterId, keyId);

  if (!clusterId) {
    return res.status(400).json({ ok: false, error: "bad_cluster_id" });
  }
  if (cloudClusters.has(clusterId)) {
    return res.status(409).json({ ok: false, error: "cloud_cluster_exists" });
  }

  cloudClusters.set(clusterId, {
    clusterId,
    keyId,
    enabled: true,
    transport: CLOUD_TRANSPORT,
    brokerHost,
    brokerPort,
    backendKey: "",
    clusterName,
    caPublicPem: caBundle.caPublicPem,
    caPrivatePem: caBundle.caPrivatePem,
    caFingerprint: caBundle.caFingerprint,
    bindingSecret: caBundle.bindingSecret,
    lastMasterNodeId: 0,
    lastMasterDeviceId: "",
    onlineDevices: 0,
    knownDevices: 0,
    firstSeenAt: nowIso(),
    lastSeenAt: nowIso(),
    lastUpdatedAt: nowIso(),
  });
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  return res.json({ ok: true, cluster: cloudClusterSummary(cloudClusters.get(clusterId)) });
});

app.get("/api/cloud/clusters/:clusterId/provision", (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  const provisioning = buildClusterProvisioning(clusterRecord);
  return res.json({
    ok: true,
    cluster: cloudClusterSummary(clusterRecord),
    provisioning,
    formFields: buildProvisioningFormFields(provisioning),
    trustBundle: {
      clusterCaPublicPem: clusterRecord.caPublicPem || "",
      clusterCaFingerprint: clusterRecord.caFingerprint || "",
      clusterBindingSecret: clusterRecord.bindingSecret || "",
    },
  });
});

app.post("/api/cloud/clusters/:clusterId/rotate-ca", (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  rotateClusterCa(clusterRecord);
  cloudClusters.set(clusterRecord.clusterId, clusterRecord);
  scheduleCloudRegistrySave();
  pushEvent("cloud-cluster-ca-rotated", {
    clusterId: clusterRecord.clusterId,
    keyId: clusterRecord.keyId,
    caFingerprint: clusterRecord.caFingerprint || "",
  });
  return res.json({
    ok: true,
    cluster: cloudClusterSummary(clusterRecord),
    provision: {
      cluster: buildClusterProvisioning(clusterRecord),
      devices: Array.from(cloudDevices.values())
        .filter((item) => item.clusterId === clusterRecord.clusterId)
        .map((item) => buildDeviceProvisioningBundle(clusterRecord, item)),
    },
  });
});

app.post("/api/cloud/clusters/:clusterId/apply-online", async (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  const targetNodes = Array.from(knownNodes.values()).filter((node) =>
    cloudClusterKeyOf(node) === String(req.params.clusterId || "").trim() &&
    node.ip);
  const results = [];

  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }

  for (const node of targetNodes) {
    try {
      const derivedDeviceId = String(node.cloudDeviceId || "").trim() ||
        deriveFallbackDeviceId(node);
      if (!String(node.cloudDeviceId || "").trim()) {
        syncCloudRegistryFromKnownNode({
          ...node,
          cloudClusterId: clusterRecord.clusterId,
          cloudDeviceId: derivedDeviceId,
          cloudKeyId: String(node.cloudKeyId || "").trim(),
        });
      }
      const deviceRecord = findCloudDeviceRecord(clusterRecord.clusterId, derivedDeviceId);
      if (!deviceRecord) {
        throw new Error("cloud_device_materialize_failed");
      }
      await applyProvisioningToNode(node, clusterRecord, deviceRecord);
      results.push({
        nodeId: Number(node.nodeId || 0),
        device: node.device || "",
        deviceId: derivedDeviceId,
        ok: true,
      });
      pushEvent("cloud-cluster-apply-online-ok", {
        clusterId: clusterRecord.clusterId,
        nodeId: Number(node.nodeId || 0),
        deviceId: derivedDeviceId,
      });
    } catch (error) {
      results.push({
        nodeId: Number(node.nodeId || 0),
        device: node.device || "",
        deviceId: String(node.cloudDeviceId || "").trim(),
        ok: false,
        error: String(error?.message || error),
      });
      pushEvent("cloud-cluster-apply-online-failed", {
        clusterId: clusterRecord.clusterId,
        nodeId: Number(node.nodeId || 0),
        error: String(error?.message || error),
      });
    }
  }

  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  return res.json({
    ok: true,
    clusterId: clusterRecord.clusterId,
    totalOnlineNodes: targetNodes.length,
    results,
  });
});

app.post("/api/cloud/devices", (req, res) => {
  const clusterId = String(req.body.clusterId || "").trim();
  const deviceId = String(req.body.deviceId || generateDeviceIdentityId()).trim();
  const deviceName = String(req.body.deviceName || deviceId).trim();
  const keyId = String(req.body.keyId || generateCloudKeyId()).trim();
  const registryKey = `${clusterId}|${deviceId}`;
  const clusterRecord = findCloudClusterRecord(clusterId);
  let deviceBundle;

  if (!clusterId) {
    return res.status(400).json({ ok: false, error: "bad_cluster_id" });
  }
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (cloudDevices.has(registryKey)) {
    /* Device exists ??update keyId and/or nodeId (e.g. MCU UID
       produces a different self_node_id than the deviceId suffix). */
    const existing = cloudDevices.get(registryKey);
    if (keyId) existing.keyId = keyId;
    if (req.body.nodeId) existing.nodeId = Number(req.body.nodeId);
    saveCloudRegistry();
    return res.json({ ok: true, updated: true, keyId: existing.keyId, nodeId: existing.nodeId });
  }

  deviceBundle = buildDeviceCredentialBundle(clusterRecord, deviceId, deviceName, keyId);

  cloudDevices.set(registryKey, {
    registryKey,
    clusterId,
    deviceId,
    keyId,
    enabled: true,
    transport: 1,
    nodeId: 0,
    device: deviceName,
    clusterName: clusterRecord.clusterName || clusterId,
    ip: "",
    role: 0,
    online: 0,
    deviceState: 0,
    relayMask: 0,
    sourceMasterNodeId: 0,
    backendKey: clusterRecord.backendKey || "",
    devicePublicPem: deviceBundle.devicePublicPem,
    devicePrivatePem: deviceBundle.devicePrivatePem,
    deviceFingerprint: deviceBundle.deviceFingerprint,
    certificatePayload: deviceBundle.certificatePayload,
    certificateSignature: deviceBundle.certificateSignature,
    firstSeenAt: nowIso(),
    lastSeenAt: "",
    lastUpdatedAt: nowIso(),
  });

  clusterRecord.knownDevices = Array.from(cloudDevices.values()).filter((item) => item.clusterId === clusterId).length;
  clusterRecord.lastUpdatedAt = nowIso();
  cloudClusters.set(clusterId, clusterRecord);

  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  return res.json({ ok: true, device: cloudDeviceSummary(cloudDevices.get(registryKey)) });
});

app.post("/api/cloud/claim-node", (req, res) => {
  const nodeId = Number(req.body.nodeId || req.body.node_id || 0);
  const clusterId = String(req.body.clusterId || req.body.cluster_id || "default").trim() || "default";
  const deviceId = String(req.body.deviceId || req.body.device_id || (nodeId ? `node-${nodeId}` : "")).trim();
  const deviceName = String(req.body.deviceName || req.body.device_name || deviceId || `Node ${nodeId}`).trim();
  const owner = String(req.body.owner || DASHBOARD_USER).trim();
  let clusterRecord = findCloudClusterRecord(clusterId);
  let deviceRecord;

  if (!nodeId || !deviceId) {
    return res.status(400).json({ ok: false, error: "missing_node_id_or_device_id" });
  }

  if (!clusterRecord) {
    const keyId = generateCloudKeyId();
    const caBundle = buildClusterCaBundle(clusterId, keyId);
    clusterRecord = {
      clusterId,
      clusterName: clusterId,
      keyId,
      enabled: true,
      transport: CLOUD_TRANSPORT,
      brokerHost: CLOUD_BROKER_HOST,
      brokerPort: CLOUD_BROKER_PORT,
      backendKey: "",
      revision: 1,
      caRevocationTag: 0,
      ...caBundle,
      lastMasterNodeId: 0,
      lastMasterDeviceId: "",
      onlineDevices: 0,
      knownDevices: 0,
      firstSeenAt: nowIso(),
      lastSeenAt: nowIso(),
      lastUpdatedAt: nowIso(),
    };
    cloudClusters.set(clusterId, clusterRecord);
  }

  deviceRecord = findCloudDeviceRecord(clusterId, deviceId);
  if (!deviceRecord) {
    const keyId = generateCloudKeyId();
    const bundle = buildDeviceCredentialBundle(clusterRecord, deviceId, deviceName, keyId);
    deviceRecord = {
      registryKey: `${clusterId}|${deviceId}`,
      clusterId,
      deviceId,
      keyId,
      enabled: true,
      transport: 1,
      nodeId,
      device: deviceName,
      clusterName: clusterRecord.clusterName || clusterId,
      ip: "",
      role: 0,
      online: 0,
      deviceState: 0,
      relayMask: 0,
      sourceMasterNodeId: 0,
      backendKey: "",
      devicePublicPem: bundle.devicePublicPem,
      devicePrivatePem: bundle.devicePrivatePem,
      deviceFingerprint: bundle.deviceFingerprint,
      certificatePayload: bundle.certificatePayload,
      certificateSignature: bundle.certificateSignature,
      firstSeenAt: nowIso(),
      lastSeenAt: "",
      lastUpdatedAt: nowIso(),
    };
  }

  deviceRecord.nodeId = nodeId;
  deviceRecord.device = deviceName;
  deviceRecord.owner = owner;
  deviceRecord.claimedAt = deviceRecord.claimedAt || nowIso();
  deviceRecord.lastUpdatedAt = nowIso();
  cloudDevices.set(deviceRecord.registryKey, deviceRecord);
  clusterRecord.knownDevices = Array.from(cloudDevices.values()).filter((item) => item.clusterId === clusterId).length;
  clusterRecord.lastUpdatedAt = nowIso();
  cloudClusters.set(clusterId, clusterRecord);
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  pushEvent("cloud-node-claimed", { clusterId, deviceId, nodeId, owner });
  return res.json({
    ok: true,
    cluster: cloudClusterSummary(clusterRecord),
    device: cloudDeviceSummary(deviceRecord),
    provisioningHint: {
      cluster_id: clusterId,
      device_id: deviceId,
      note: "目前為測試認領；正式版建議改為簽章 challenge/response。",
    },
  });
});

app.get("/api/cloud/devices/:clusterId/:deviceId/provision", (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  const deviceRecord = findCloudDeviceRecord(req.params.clusterId, req.params.deviceId);
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (!deviceRecord) {
    return res.status(404).json({ ok: false, error: "cloud_device_not_found" });
  }
  const provisioning = buildDeviceProvisioning(clusterRecord, deviceRecord);
  const bundle = buildDeviceProvisioningBundle(clusterRecord, deviceRecord);
  return res.json({
    ok: true,
    cluster: bundle.cluster,
    device: bundle.device,
    provisioning,
    formFields: buildProvisioningFormFields(provisioning),
    trustBundle: bundle.trustBundle,
    deviceBundle: bundle.deviceBundle,
    privateKeyExportEnabled: bundle.privateKeyExportEnabled,
  });
});

app.post("/api/cloud/devices/:clusterId/:deviceId/rotate-credential", (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  const deviceRecord = findCloudDeviceRecord(req.params.clusterId, req.params.deviceId);
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (!deviceRecord) {
    return res.status(404).json({ ok: false, error: "cloud_device_not_found" });
  }
  rotateDeviceCredential(clusterRecord, deviceRecord);
  cloudDevices.set(deviceRecord.registryKey, deviceRecord);
  scheduleCloudRegistrySave();
  pushEvent("cloud-device-credential-rotated", {
    clusterId: deviceRecord.clusterId,
    deviceId: deviceRecord.deviceId,
    keyId: deviceRecord.keyId,
    deviceFingerprint: deviceRecord.deviceFingerprint || "",
  });
  return res.json({
    ok: true,
    bundle: buildDeviceProvisioningBundle(clusterRecord, deviceRecord),
  });
});

app.post("/api/cloud/apply/:nodeId", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const clusterId = String(req.body.clusterId || "").trim();
  const deviceId = String(req.body.deviceId || "").trim();
  const node = resolveKnownNode(nodeId);
  const clusterRecord = findCloudClusterRecord(clusterId);
  const deviceRecord = findCloudDeviceRecord(clusterId, deviceId);

  if (!node || !node.ip || !node.online) {
    return res.status(404).json({ ok: false, error: "node_offline" });
  }
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (!deviceRecord) {
    return res.status(404).json({ ok: false, error: "cloud_device_not_found" });
  }

  try {
    await applyProvisioningToNode(node, clusterRecord, deviceRecord);
  } catch (error) {
    return res.status(502).json({ ok: false, error: "remote_apply_failed", detail: String(error.message || error) });
  }

  pushEvent("cloud-provision-applied", {
    nodeId,
    ip: node.ip,
    clusterId,
    deviceId,
  });
  return res.json({
    ok: true,
    nodeId,
    clusterId,
    deviceId,
    appliedFields: buildDeviceProvisioning(clusterRecord, deviceRecord),
  });
});

app.patch("/api/cloud/devices/:clusterId/:deviceId", (req, res) => {
  const record = findCloudDeviceRecord(req.params.clusterId, req.params.deviceId);
  if (!record) {
    return res.status(404).json({ ok: false, error: "cloud_device_not_found" });
  }
  if (typeof req.body.enabled === "boolean") {
    record.enabled = req.body.enabled;
  }
  record.lastUpdatedAt = nowIso();
  cloudDevices.set(record.registryKey, record);
  if (record.enabled === false) {
    for (const client of Array.from(clients.values())) {
      if ((client.cloudClusterId || "") === record.clusterId && (client.cloudDeviceId || "") === record.deviceId) {
        pushEvent("cloud-device-disabled-disconnect", {
          nodeId: client.nodeId,
          device: client.device,
          clusterId: record.clusterId,
          deviceId: record.deviceId,
        });
        client.socket.destroy();
      }
    }
  }
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  return res.json({ ok: true, device: cloudDeviceSummary(record) });
});

app.post("/api/cloud/devices/from-node", (req, res) => {
  const nodeId = Number(req.body.nodeId || 0);
  const node = resolveKnownNode(nodeId);
  const requestedClusterId = String(req.body.clusterId || "").trim();
  const clusterId = requestedClusterId || String(node?.cloudClusterId || "").trim() || String(node?.cluster || "").trim();
  const clusterRecord = findCloudClusterRecord(clusterId);
  const requestedDeviceId = String(req.body.deviceId || "").trim();
  const deviceId = requestedDeviceId || deriveFallbackDeviceId(node || { nodeId });
  const registryKey = `${clusterId}|${deviceId}`;
  const existing = cloudDevices.get(registryKey) || {};
  const keyId = String(req.body.keyId || existing.keyId || generateCloudKeyId()).trim();
  const seenAt = nowIso();
  let record;
  let deviceBundle;

  if (!node || !node.nodeId) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  if (!clusterId) {
    return res.status(400).json({ ok: false, error: "bad_cluster_id" });
  }
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (!deviceId) {
    return res.status(400).json({ ok: false, error: "bad_device_id" });
  }

  deviceBundle = existing.devicePrivatePem ? {
    devicePublicPem: existing.devicePublicPem || "",
    devicePrivatePem: existing.devicePrivatePem || "",
    deviceFingerprint: existing.deviceFingerprint || "",
    certificatePayload: existing.certificatePayload || null,
    certificateSignature: existing.certificateSignature || "",
  } : buildDeviceCredentialBundle(clusterRecord, deviceId, node?.device || deviceId, keyId);

  record = {
    ...existing,
    registryKey,
    clusterId,
    deviceId,
    keyId,
    enabled: existing.enabled !== false,
    transport: Number(node.cloudTransport || clusterRecord.transport || CLOUD_TRANSPORT),
    nodeId: Number(node.nodeId || 0),
    device: node.device || existing.device || deviceId,
    clusterName: node.cluster || clusterRecord.clusterName || clusterId,
    ip: normalizeIp(node.ip),
    role: Number(node.role || 0),
    online: Number(node.online ? 1 : 0),
    deviceState: Number(node.deviceState || 0),
    relayMask: Number(node.relayMask || 0) >>> 0,
    sourceMasterNodeId: Number(node.sourceMasterNodeId || 0),
    backendKey: node.backendKey || clusterRecord.backendKey || "",
    devicePublicPem: deviceBundle.devicePublicPem,
    devicePrivatePem: deviceBundle.devicePrivatePem,
    deviceFingerprint: deviceBundle.deviceFingerprint,
    certificatePayload: deviceBundle.certificatePayload,
    certificateSignature: deviceBundle.certificateSignature,
    firstSeenAt: existing.firstSeenAt || seenAt,
    lastSeenAt: node.lastSeenAt || seenAt,
  };

  cloudDevices.set(registryKey, record);
  syncCloudRegistryFromKnownNode({
    ...node,
    cloudClusterId: clusterId,
    cloudDeviceId: deviceId,
    cloudKeyId: keyId,
  });
  scheduleCloudRegistrySave();
  pushEvent("cloud-device-created-from-node", {
    nodeId: Number(node.nodeId || 0),
    clusterId,
    deviceId,
    keyId,
  });
  return res.json({ ok: true, device: cloudDeviceSummary(record) });
});

app.patch("/api/cloud/clusters/:clusterId", (req, res) => {
  const record = findCloudClusterRecord(req.params.clusterId);
  if (!record) {
    return res.status(404).json({ ok: false, error: "cloud_cluster_not_found" });
  }
  if (typeof req.body.enabled === "boolean") {
    record.enabled = req.body.enabled;
  }
  if (typeof req.body.transport !== "undefined") {
    const transport = Number(req.body.transport);
    if (!Number.isInteger(transport) || transport < 1 || transport > 255) {
      return res.status(400).json({ ok: false, error: "bad_cloud_transport" });
    }
    record.transport = transport;
  }
  if (typeof req.body.brokerHost !== "undefined") {
    const brokerHost = String(req.body.brokerHost || "").trim();
    if (!brokerHost || brokerHost.length > 253) {
      return res.status(400).json({ ok: false, error: "bad_broker_host" });
    }
    record.brokerHost = brokerHost;
  }
  if (typeof req.body.brokerPort !== "undefined") {
    const brokerPort = Number(req.body.brokerPort);
    if (!Number.isInteger(brokerPort) || brokerPort < 1 || brokerPort > 65535) {
      return res.status(400).json({ ok: false, error: "bad_broker_port" });
    }
    record.brokerPort = brokerPort;
  }
  record.lastUpdatedAt = nowIso();
  cloudClusters.set(record.clusterId, record);
  if (record.enabled === false) {
    for (const client of Array.from(clients.values())) {
      if ((client.cloudClusterId || "") === record.clusterId) {
        pushEvent("cloud-cluster-disabled-disconnect", {
          nodeId: client.nodeId,
          device: client.device,
          clusterId: record.clusterId,
        });
        client.socket.destroy();
      }
    }
  }
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  return res.json({ ok: true, cluster: cloudClusterSummary(record) });
});

app.get("/clients", async (_req, res) => {
  try {
    const response = await fetch(`${BROKER_CONTROL_URL}/api/clients`);
    const data = await response.json();
    res.json(data);
  } catch (_) {
    res.json({ ok: true, clients: [] });
  }
});

app.get("/api/clients", async (_req, res) => {
  try {
    const response = await fetch(`${BROKER_CONTROL_URL}/api/clients`);
    const data = await response.json();
    res.json(data);
  } catch (_) {
    res.json({ ok: true, clients: [] });
  }
});

app.get("/events", (_req, res) => {
  res.json({
    ok: true,
    events,
  });
});

app.get("/api/events", (_req, res) => {
  res.json({
    ok: true,
    events,
  });
});

app.get("/api/config/:nodeId/relays", async (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const node = resolveKnownNode(nodeId);
  let data;

  if (!node || !node.ip || !node.online) {
    return res.json({ ok: true, relays: [] });
  }

  try {
    data = await remoteNodeGetJson(nodeId, "/api/config/relays");
    if (!data || !data.ok) {
      return res.status(502).json({ ok: false, error: (data && data.error) || "remote_fetch_failed" });
    }
    return res.json(data);
  } catch (_error) {
    return res.status(502).json({
      ok: false,
      error: _error.message || "remote_fetch_failed",
    });
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
  if (typeof req.body.adc_channel !== "undefined") {
    /* Backward compatibility only.  MCU firmware now uses fixed CHn -> ADCn
       mapping, so the backend must not persist or forward user-selected ADC
       bindings from older UI/API clients. */
    void req.body.adc_channel;
  }
  form.set("adc_channel", String(channel - 1));
  if (typeof req.body.protection_enabled !== "undefined") {
    form.set("protection_enabled", Number(req.body.protection_enabled) ? "1" : "0");
  }
  if (typeof req.body.device_strategy !== "undefined") {
    const strategy = Number(req.body.device_strategy);
    if (![0, 1, 2].includes(strategy)) return res.status(400).json({ ok: false, error: "bad_strategy" });
    form.set("device_strategy", String(strategy));
  }
  for (const field of [
    "overcurrent_milliamps",
    "trip_delay_ms",
    "reset_hysteresis_milliamps",
  ]) {
    if (typeof req.body[field] !== "undefined") {
      const fieldValue = Number(req.body[field]);
      const minimum = field === "overcurrent_milliamps" ? 100 : 0;
      const maximum = field === "trip_delay_ms" ? 600000 : 100000;
      if (!Number.isInteger(fieldValue) || fieldValue < minimum || fieldValue > maximum) {
        return res.status(400).json({ ok: false, error: `bad_${field}` });
      }
      form.set(field, String(fieldValue));
    }
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

function validateRemoteNodeId(nodeId) {
  const nid = Number(nodeId);
  const node = resolveKnownNode(nid);
  if (!Number.isFinite(nid) || nid <= 0) return { error: "bad_node_id" };
  if (!node || !node.ip || !node.online) return { error: "node_offline", status: 404 };
  return { nid, node };
}

function appendProtectionConfigForm(form, body) {
  for (const field of [
    "cooldown_seconds",
    "retry_delay_seconds",
    "max_retry_count",
    "boot_delay_seconds",
    "learning_samples",
    "aging_update_hours",
    "aging_cap_percent",
    "stall_multiplier_x10",
  ]) {
    if (typeof body[field] !== "undefined") {
      const value = Number(body[field]);
      if (!Number.isInteger(value) || value < 0) return `bad_${field}`;
      form.set(field, String(value));
    }
  }
  return "";
}

app.get("/api/cluster/node/:nodeId/config/relays", async (req, res) => {
  const v = validateRemoteNodeId(req.params.nodeId);
  if (v.error) return res.status(v.status || 400).json({ ok: false, error: v.error });
  try {
    const data = await remoteNodeGetJson(v.nid, "/api/config/relays");
    return res.json(data);
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/cluster/node/:nodeId/config/relay", async (req, res) => {
  req.params.nodeId = String(req.params.nodeId);
  req.url = `/api/config/${encodeURIComponent(req.params.nodeId)}/relay`;
  const nodeId = Number(req.params.nodeId);
  const channel = Number(req.body.channel);
  const form = new URLSearchParams();
  let data;

  if (!Number.isInteger(channel) || channel < 1 || channel > 16) {
    return res.status(400).json({ ok: false, error: "bad_channel" });
  }
  if (validateRemoteNodeId(nodeId).error) {
    const v = validateRemoteNodeId(nodeId);
    return res.status(v.status || 400).json({ ok: false, error: v.error });
  }

  form.set("channel", String(channel));
  for (const field of ["name"]) {
    if (typeof req.body[field] === "string") form.set(field, req.body[field]);
  }
  for (const field of ["enabled", "remember_state", "boot_default_on", "protection_enabled"]) {
    if (typeof req.body[field] !== "undefined") form.set(field, Number(req.body[field]) ? "1" : "0");
  }
  if (typeof req.body.device_strategy !== "undefined") {
    const strategy = Number(req.body.device_strategy);
    if (![0, 1, 2].includes(strategy)) return res.status(400).json({ ok: false, error: "bad_strategy" });
    form.set("device_strategy", String(strategy));
  }
  if (typeof req.body.adc_channel !== "undefined") {
    /* Backward compatibility only; see /api/config/:nodeId/relay above. */
    void req.body.adc_channel;
  }
  form.set("adc_channel", String(channel - 1));
  for (const field of ["overcurrent_milliamps", "trip_delay_ms", "reset_hysteresis_milliamps"]) {
    if (typeof req.body[field] !== "undefined") {
      const value = Number(req.body[field]);
      const minimum = field === "overcurrent_milliamps" ? 100 : 0;
      const maximum = field === "trip_delay_ms" ? 600000 : 100000;
      if (!Number.isInteger(value) || value < minimum || value > maximum) {
        return res.status(400).json({ ok: false, error: `bad_${field}` });
      }
      form.set(field, String(value));
    }
  }

  try {
    data = await remoteNodeRequest(nodeId, "/api/config/relay", "POST", form);
    if (!data || data.ok === false) {
      return res.status(502).json({ ok: false, error: (data && data.error) || "remote_config_failed" });
    }
    return res.json(data);
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.get("/api/cluster/node/:nodeId/protection", async (req, res) => {
  const v = validateRemoteNodeId(req.params.nodeId);
  if (v.error) return res.status(v.status || 400).json({ ok: false, error: v.error });
  try {
    return res.json(await remoteNodeGetJson(v.nid, "/api/protection"));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.get("/api/cluster/node/:nodeId/config/protection", async (req, res) => {
  const v = validateRemoteNodeId(req.params.nodeId);
  if (v.error) return res.status(v.status || 400).json({ ok: false, error: v.error });
  try {
    return res.json(await remoteNodeGetJson(v.nid, "/api/config/protection"));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/cluster/node/:nodeId/config/protection", async (req, res) => {
  const v = validateRemoteNodeId(req.params.nodeId);
  const form = new URLSearchParams();
  let formError;
  if (v.error) return res.status(v.status || 400).json({ ok: false, error: v.error });
  formError = appendProtectionConfigForm(form, req.body || {});
  if (formError) return res.status(400).json({ ok: false, error: formError });
  try {
    return res.json(await remoteNodeRequest(v.nid, "/api/config/protection", "POST", form));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/cluster/node/:nodeId/protection/:action", async (req, res) => {
  const v = validateRemoteNodeId(req.params.nodeId);
  const action = String(req.params.action || "");
  const form = new URLSearchParams();
  const channel = Number(req.body.channel);
  if (v.error) return res.status(v.status || 400).json({ ok: false, error: v.error });
  if (!["unlock", "force-stop", "clear-learning", "strategy"].includes(action)) {
    return res.status(404).json({ ok: false, error: "bad_action" });
  }
  if (!Number.isInteger(channel) || channel < 1 || channel > 16) {
    return res.status(400).json({ ok: false, error: "bad_channel" });
  }
  form.set("channel", String(channel));
  if (action === "strategy") {
    const strategy = Number(req.body.strategy);
    if (![0, 1, 2].includes(strategy)) return res.status(400).json({ ok: false, error: "bad_strategy" });
    form.set("strategy", String(strategy));
  }
  try {
    return res.json(await remoteNodeRequest(v.nid, `/api/protection/${action}`, "POST", form));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

/* ?? Scheduler proxy APIs (dashboard ??backend ??MCU HTTP) ?? */
function validateSchedulerParams(nodeId, channel) {
  const nid = Number(nodeId);
  const ch = Number(channel);
  if (!Number.isFinite(nid) || nid <= 0) return { error: "bad_node_id" };
  if (!Number.isInteger(ch) || ch < 1 || ch > 16) return { error: "bad_channel" };
  return { nid, ch };
}
function validateWeekday(wd) {
  const w = Number(wd);
  if (!Number.isInteger(w) || w < 0 || w > 6) return null;
  return w;
}

app.get("/api/scheduler/:nodeId/:channel/status", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    const data = await remoteNodeGetJson(v.nid, `/api/relay/channels/${v.ch}/scheduler/status`);
    res.json(data);
  } catch (error) {
    res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.get("/api/scheduler/:nodeId/:channel/week", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    const data = await remoteNodeGetJson(v.nid, `/api/relay/channels/${v.ch}/schedule-week`);
    res.json(data);
  } catch (error) {
    res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/scheduler/:nodeId/:channel/enabled", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    const form = new URLSearchParams();
    form.set("enabled", Number(req.body.enabled) ? "1" : "0");
    const data = await remoteNodeRequest(v.nid, `/api/relay/channels/${v.ch}/scheduler/enabled`, "PATCH", form);
    res.json(data);
  } catch (error) {
    res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/scheduler/:nodeId/:channel/pause", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    const form = new URLSearchParams();
    form.set("mode", String(req.body.mode || 0));
    if (req.body.duration_seconds) form.set("duration_seconds", String(req.body.duration_seconds));
    if (req.body.until) form.set("until", String(req.body.until));
    form.set("pause_blocks_auto_trigger", req.body.pause_blocks_auto_trigger ? "1" : "0");
    const data = await remoteNodeRequest(v.nid, `/api/relay/channels/${v.ch}/scheduler/pause`, "POST", form);
    res.json(data);
  } catch (error) {
    res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/scheduler/:nodeId/:channel/resume", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    const data = await remoteNodeRequest(v.nid, `/api/relay/channels/${v.ch}/scheduler/resume`, "POST", new URLSearchParams());
    res.json(data);
  } catch (error) {
    res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/scheduler/:nodeId/:channel/copy", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  const srcWd = validateWeekday(req.body.source_weekday);
  if (srcWd === null) return res.status(400).json({ ok: false, error: "bad_source_weekday" });
  try {
    const form = new URLSearchParams();
    form.set("target_weekdays", String(req.body.target_weekdays || ""));
    form.set("mode", "overwrite");
    const data = await remoteNodeRequest(v.nid, `/api/relay/channels/${v.ch}/schedule-week/${srcWd}/copy`, "POST", form);
    res.json(data);
  } catch (error) {
    res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.put("/api/scheduler/:nodeId/:channel/week/:weekday", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  const wd = validateWeekday(req.params.weekday);
  const form = new URLSearchParams();
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  if (wd === null) return res.status(400).json({ ok: false, error: "bad_weekday" });
  form.set("count", String(Number(req.body.count || 0)));
  for (let i = 0; i < 8; i++) {
    if (typeof req.body[`s${i}`] !== "undefined") form.set(`s${i}`, String(Number(req.body[`s${i}`])));
    if (typeof req.body[`e${i}`] !== "undefined") form.set(`e${i}`, String(Number(req.body[`e${i}`])));
  }
  try {
    const data = await remoteNodeRequest(v.nid, `/api/relay/channels/${v.ch}/schedule-week/${wd}`, "PUT", form);
    res.json(data);
  } catch (error) {
    res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.get("/api/cluster/node/:nodeId/relay/channels/:channel/scheduler/status", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    return res.json(await remoteNodeGetJson(v.nid, `/api/relay/channels/${v.ch}/scheduler/status`));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.get("/api/cluster/node/:nodeId/relay/channels/:channel/schedule-week", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    return res.json(await remoteNodeGetJson(v.nid, `/api/relay/channels/${v.ch}/schedule-week`));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.put("/api/cluster/node/:nodeId/relay/channels/:channel/schedule-week/:weekday", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  const wd = validateWeekday(req.params.weekday);
  const form = new URLSearchParams();
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  if (wd === null) return res.status(400).json({ ok: false, error: "bad_weekday" });
  form.set("count", String(Number(req.body.count || 0)));
  for (let i = 0; i < 8; i++) {
    if (typeof req.body[`s${i}`] !== "undefined") form.set(`s${i}`, String(Number(req.body[`s${i}`])));
    if (typeof req.body[`e${i}`] !== "undefined") form.set(`e${i}`, String(Number(req.body[`e${i}`])));
  }
  try {
    return res.json(await remoteNodeRequest(v.nid, `/api/relay/channels/${v.ch}/schedule-week/${wd}`, "PUT", form));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/cluster/node/:nodeId/relay/channels/:channel/schedule-week/:weekday/copy", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  const srcWd = validateWeekday(req.params.weekday);
  const form = new URLSearchParams();
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  if (srcWd === null) return res.status(400).json({ ok: false, error: "bad_source_weekday" });
  form.set("target_weekdays", String(req.body.target_weekdays || ""));
  form.set("mode", String(req.body.mode || "overwrite"));
  try {
    return res.json(await remoteNodeRequest(v.nid, `/api/relay/channels/${v.ch}/schedule-week/${srcWd}/copy`, "POST", form));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/cluster/node/:nodeId/relay/channels/:channel/scheduler/enabled", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  const form = new URLSearchParams();
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  form.set("enabled", Number(req.body.enabled) ? "1" : "0");
  try {
    return res.json(await remoteNodeRequest(v.nid, `/api/relay/channels/${v.ch}/scheduler/enabled`, "PATCH", form));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/cluster/node/:nodeId/relay/channels/:channel/scheduler/pause", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  const form = new URLSearchParams();
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  form.set("mode", String(req.body.mode || 0));
  if (req.body.duration_seconds) form.set("duration_seconds", String(req.body.duration_seconds));
  if (req.body.until) form.set("until", String(req.body.until));
  form.set("pause_blocks_auto_trigger", req.body.pause_blocks_auto_trigger ? "1" : "0");
  try {
    return res.json(await remoteNodeRequest(v.nid, `/api/relay/channels/${v.ch}/scheduler/pause`, "POST", form));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/api/cluster/node/:nodeId/relay/channels/:channel/scheduler/resume", async (req, res) => {
  const v = validateSchedulerParams(req.params.nodeId, req.params.channel);
  if (v.error) return res.status(400).json({ ok: false, error: v.error });
  try {
    return res.json(await remoteNodeRequest(v.nid, `/api/relay/channels/${v.ch}/scheduler/resume`, "POST", new URLSearchParams()));
  } catch (error) {
    return res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/send/:nodeId/relay", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const channel = Number(req.body.channel);
  const value = Number(req.body.value);

  if (!Number.isInteger(channel) || channel < 1 || channel > 16 || ![0, 1].includes(value)) {
    return res.status(400).json({ ok: false, error: "bad_relay_request" });
  }

  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  const payload = {
    channel, value,
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  if (!mqttPublishCommand(nodeInfo.cluster, "relay", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  {
    const bit = 1 << (channel - 1);
    const oldMask = Number(nodeInfo.relayMask || 0) >>> 0;
    const nextMask = value ? (oldMask | bit) : (oldMask & ~bit);
    setKnownNodeRelayMask(Number(payload.node_id), nextMask);
  }
  queueRelayStateConfirm(Number(payload.node_id));
  pushEvent("send-relay", { nodeId, channel, value, targetNodeId: payload.node_id });
  return res.json({ ok: true });
});

app.post("/send/:nodeId/mask", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const mask = Number(req.body.mask);
  if (!Number.isInteger(mask) || mask < 0 || mask > 0xffff) {
    return res.status(400).json({ ok: false, error: "bad_mask" });
  }
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  const payload = {
    mask,
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  if (!mqttPublishCommand(nodeInfo.cluster, "mask", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  setKnownNodeRelayMask(Number(payload.node_id), mask);
  queueRelayStateConfirm(Number(payload.node_id));
  pushEvent("send-mask", { nodeId, mask, targetNodeId: payload.node_id });
  return res.json({ ok: true });
});

app.post("/send/:nodeId/all", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const value = Number(req.body.value);
  if (![0, 1].includes(value)) {
    return res.status(400).json({ ok: false, error: "bad_value" });
  }
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) {
    return res.status(404).json({ ok: false, error: "node_not_found" });
  }
  const payload = {
    mask: (value === 1) ? 0xffff : 0,
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  if (!mqttPublishCommand(nodeInfo.cluster, "mask", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  setKnownNodeRelayMask(Number(payload.node_id), payload.mask);
  queueRelayStateConfirm(Number(payload.node_id));
  pushEvent("send-all", { nodeId, value, targetNodeId: payload.node_id });
  return res.json({ ok: true });
});

/* ?? New MQTT-based command routes ?? */
app.post("/send/:nodeId/auto-hold", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    target: Number(req.body.target || 0),
    duration_seconds: Number(req.body.duration_seconds || 0),
    session: Number(req.body.session || 0),
    command_ts: Number(req.body.command_ts || Math.floor(Date.now() / 1000)),
    reason: String(req.body.reason || "").slice(0, 63),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  if (!mqttPublishCommand(nodeInfo.cluster, "auto-hold", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  return res.json({ ok: true });
});

app.post("/send/:nodeId/control", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    target: Number(req.body.target || 0),
    pause_mode: Number(req.body.pause_mode || 0),
    pause_duration_seconds: Number(req.body.pause_duration_seconds || 0),
    pause_until: Number(req.body.pause_until || 0),
    pause_blocks_auto_trigger: Number(req.body.pause_blocks_auto_trigger || 0),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  if (!mqttPublishCommand(nodeInfo.cluster, "control", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  return res.json({ ok: true });
});

app.post("/send/:nodeId/scheduler/set", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    weekday: Number(req.body.weekday || 0),
    count: Number(req.body.count || 0),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  for (let i = 0; i < 8; i++) {
    payload[`s${i}`] = Number(req.body[`s${i}`] || 0);
    payload[`e${i}`] = Number(req.body[`e${i}`] || 0);
  }
  if (!mqttPublishCommand(nodeInfo.cluster, "scheduler/set", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  return res.json({ ok: true });
});

app.post("/send/:nodeId/scheduler/enabled", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    enabled: Number(req.body.enabled || 0),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  if (!mqttPublishCommand(nodeInfo.cluster, "scheduler/enabled", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  return res.json({ ok: true });
});

app.post("/send/:nodeId/scheduler/pause", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    mode: Number(req.body.mode || 0),
    duration_seconds: Number(req.body.duration_seconds || 0),
    until: Number(req.body.until || 0),
    pause_blocks_auto_trigger: Number(req.body.pause_blocks_auto_trigger || 0),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  if (!mqttPublishCommand(nodeInfo.cluster, "scheduler/pause", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  return res.json({ ok: true });
});

app.post("/send/:nodeId/scheduler/resume", async (req, res) => {
  // Firmware has no MQTT scheduler/resume handler. Proxy via MCU HTTP instead.
  try {
    const data = await remoteNodeRequest(
      Number(req.params.nodeId),
      `/api/relay/channels/${Number(req.body.channel || 1)}/scheduler/resume`,
      "POST",
      new URLSearchParams()
    );
    res.json(data);
  } catch (error) {
    res.status(502).json({ ok: false, error: error.message || "proxy_failed" });
  }
});

app.post("/send/:nodeId/protection/force-stop", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const channel = Number(req.body.channel || 1);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  if (!Number.isInteger(channel) || channel < 1 || channel > 16) {
    return res.status(400).json({ ok: false, error: "bad_channel" });
  }
  const payload = {
    channel,
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  if (!mqttPublishCommand(nodeInfo.cluster, "protection/force-stop", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  setKnownNodeRelayMask(Number(payload.node_id), (Number(nodeInfo.relayMask || 0) >>> 0) & ~(1 << (channel - 1)));
  return res.json({ ok: true });
});

app.post("/send/:nodeId/protection/unlock", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  if (!mqttPublishCommand(nodeInfo.cluster, "protection/unlock", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  return res.json({ ok: true });
});

app.post("/send/:nodeId/protection/clear-learning", (req, res) => {
  const nodeId = Number(req.params.nodeId);
  const nodeInfo = resolveKnownNode(nodeId);
  if (!nodeInfo || !nodeInfo.cluster) return res.status(404).json({ ok: false, error: "node_not_found" });
  const payload = {
    channel: Number(req.body.channel || 1),
    node_id: Number(req.body.targetNodeId || nodeId),
    request_id: crypto.randomBytes(8).toString("hex"),
  };
  if (!mqttPublishCommand(nodeInfo.cluster, "protection/clear-learning", payload)) {
    return res.status(502).json({ ok: false, error: "mqtt_unavailable" });
  }
  return res.json({ ok: true });
});

// ?? MCU Provisioning API ??????????????????????????????????????????
// Path A in the dual-path architecture: the MCU calls this over HTTPS
// (through Cloudflare) to obtain its PSK and broker configuration.
//
// MCU sends:   POST /api/provision
//              { device_id, cluster_id, device_fingerprint }
//
// Server replies with binding_secret, broker host/port, CA fingerprint,
// and CA root cert PEM so the MCU can verify the MQTT broker on connect.

app.post("/api/provision", (req, res) => {
  const clusterId = String(req.body.cluster_id || "").trim();
  const deviceId = String(req.body.device_id || "").trim();
  const deviceFingerprint = String(req.body.device_fingerprint || "").trim();

  if (!clusterId || !deviceId) {
    return res.status(400).json({ ok: false, error: "missing_cluster_or_device_id" });
  }

  const deviceRecord = findCloudDeviceRecord(clusterId, deviceId);
  if (!deviceRecord) {
    return res.status(404).json({ ok: false, error: "device_not_registered" });
  }

  if (!deviceRecord.enabled) {
    return res.status(403).json({ ok: false, error: "device_disabled" });
  }

  /* Devices must be claimed by a user before they can provision.
     An unclaimed device is registered but has no owner ??it cannot
     obtain a PSK and therefore cannot connect to the MQTT broker. */
  if (!deviceRecord.owner || String(deviceRecord.owner).trim() === "") {
    return res.status(403).json({ ok: false, error: "device_not_claimed" });
  }

  // Device fingerprint is trust-on-first-use for MCU provisioning.
  //
  // Older dashboard-created records may contain a backend-generated
  // deviceFingerprint from an unused device-key bundle.  That value is not the
  // MCU fingerprint and must not block the first MCU provision request.
  const boundFingerprint = String(deviceRecord.mcuDeviceFingerprint || "").trim();
  const fingerprintSource = String(deviceRecord.deviceFingerprintSource || "").trim();
  if (deviceFingerprint) {
    if (boundFingerprint) {
      if (deviceFingerprint !== boundFingerprint) {
        return res.status(403).json({ ok: false, error: "fingerprint_mismatch" });
      }
    } else if (fingerprintSource === "mcu" && deviceRecord.deviceFingerprint) {
      if (deviceFingerprint !== String(deviceRecord.deviceFingerprint || "")) {
        return res.status(403).json({ ok: false, error: "fingerprint_mismatch" });
      }
    } else {
      deviceRecord.mcuDeviceFingerprint = deviceFingerprint;
      deviceRecord.deviceFingerprintSource = "mcu";
      deviceRecord.fingerprintBoundAt = nowIso();
      deviceRecord.lastUpdatedAt = nowIso();
      cloudDevices.set(deviceRecord.registryKey, deviceRecord);
      scheduleCloudRegistrySave();
      pushEvent("device-fingerprint-bound", {
        clusterId,
        deviceId,
        fingerprint: deviceFingerprint,
      });
    }
  }

  const clusterRecord = findCloudClusterRecord(clusterId);
  if (!clusterRecord) {
    return res.status(500).json({ ok: false, error: "cluster_not_found" });
  }

  pushEvent("mcu-provision-request", {
    clusterId,
    deviceId,
    ip: req.ip,
    timestamp: new Date().toISOString(),
  });

  return res.json({
    ok: true,
    cluster_id: clusterId,
    device_id: deviceId,
    cloud_key_id: deviceRecord.keyId || "",
    cloud_transport: clusterRecord.transport || CLOUD_TRANSPORT,
    cloud_broker_host: clusterRecord.brokerHost || CLOUD_BROKER_HOST,
    cloud_broker_port: clusterRecord.brokerPort || CLOUD_BROKER_PORT,
    cloud_cluster_ca_fingerprint: clusterRecord.caFingerprint || "",
    cloud_cluster_ca_cert_pem: clusterRecord.caPublicPem || "",
    cloud_cluster_binding_secret: clusterRecord.bindingSecret || "",
    provisioning_revision: Number(clusterRecord.revision || 0),
    ca_revocation_tag: Number(clusterRecord.caRevocationTag || 0),
  });
});

// ?? Device Claim API ??????????????????????????????????????????????
// A user claims ownership of a registered device.  Until claimed,
// the device cannot obtain a PSK through /api/provision and cannot
// connect to the MQTT broker.

app.post("/api/devices/:deviceId/claim", (req, res) => {
  const deviceId = String(req.params.deviceId || "").trim();
  const owner = String(req.body.owner || "").trim();

  if (!deviceId || !owner) {
    return res.status(400).json({ ok: false, error: "missing_device_id_or_owner" });
  }

  let foundRecord = null;
  for (const [key, record] of Array.from(cloudDevices.entries())) {
    if (String(record.deviceId || "").trim() === deviceId) {
      foundRecord = record;
      break;
    }
  }
  if (!foundRecord) {
    return res.status(404).json({ ok: false, error: "device_not_found" });
  }

  const currentOwner = String(foundRecord.owner || "").trim();
  if (currentOwner && currentOwner !== owner) {
    return res.status(403).json({ ok: false, error: "device_already_claimed", currentOwner });
  }

  foundRecord.owner = owner;
  cloudDevices.set(foundRecord.registryKey, foundRecord);
  scheduleCloudRegistrySave();
  pushEvent("device-claimed", { deviceId, owner, timestamp: new Date().toISOString() });
  return res.json({ ok: true, deviceId, owner });
});

app.post("/api/cloud/devices/:clusterId/:deviceId/reset-fingerprint", (req, res) => {
  const clusterId = String(req.params.clusterId || "").trim();
  const deviceId = String(req.params.deviceId || "").trim();
  const record = findCloudDeviceRecord(clusterId, deviceId);

  if (!record) {
    return res.status(404).json({ ok: false, error: "cloud_device_not_found" });
  }

  delete record.mcuDeviceFingerprint;
  delete record.deviceFingerprintSource;
  delete record.fingerprintBoundAt;
  record.lastUpdatedAt = nowIso();
  cloudDevices.set(record.registryKey, record);
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  pushEvent("device-fingerprint-reset", { clusterId, deviceId });
  return res.json({ ok: true, device: cloudDeviceSummary(record) });
});

app.post("/api/devices/:deviceId/reset-fingerprint", (req, res) => {
  const deviceId = String(req.params.deviceId || "").trim();
  let foundRecord = null;

  for (const record of cloudDevices.values()) {
    if (String(record.deviceId || "").trim() === deviceId) {
      foundRecord = record;
      break;
    }
  }
  if (!foundRecord) {
    return res.status(404).json({ ok: false, error: "device_not_found" });
  }

  delete foundRecord.mcuDeviceFingerprint;
  delete foundRecord.deviceFingerprintSource;
  delete foundRecord.fingerprintBoundAt;
  foundRecord.lastUpdatedAt = nowIso();
  cloudDevices.set(foundRecord.registryKey, foundRecord);
  scheduleCloudRegistrySave();
  scheduleStateBroadcast();
  pushEvent("device-fingerprint-reset", {
    clusterId: foundRecord.clusterId,
    deviceId: foundRecord.deviceId,
  });
  return res.json({ ok: true, deviceId: foundRecord.deviceId });
});

// ?? PSK Rotation ??????????????????????????????????????????????????
// Increments the cluster revision, generates a new binding secret,
// and stores the old secret for a 5-minute transition window.
app.post("/api/cloud/clusters/:clusterId/rotate-psk", (req, res) => {
  const clusterRecord = findCloudClusterRecord(req.params.clusterId);
  if (!clusterRecord) {
    return res.status(404).json({ ok: false, error: "cluster_not_found" });
  }

  const oldSecret = clusterRecord.bindingSecret || "";
  clusterRecord.bindingSecret = crypto.randomBytes(32).toString("base64url").slice(0, 43);
  clusterRecord.previousSecret = oldSecret;
  clusterRecord.revision = (Number(clusterRecord.revision || 0)) + 1;
  clusterRecord.secretRotatedAt = Date.now();

  cloudClusters.set(clusterRecord.clusterId, clusterRecord);
  scheduleCloudRegistrySave();
  pushEvent("psk-rotated", {
    clusterId: clusterRecord.clusterId,
    revision: clusterRecord.revision,
    timestamp: new Date().toISOString(),
  });
  return res.json({
    ok: true,
    revision: clusterRecord.revision,
    rotatedAt: clusterRecord.secretRotatedAt,
  });
});

loadKeyStore();
loadCloudRegistry();

/* Commands are published to the MQTT broker via its control API on port 4301 */

/* ????????????????????????????????????????????????????????????????
   Sensor Auto Hold API ??Grafana/backend sensor alerts ??MCU
   ????????????????????????????????????????????????????????????????*/

// activeSensors: key = "cluster|device|channel"
const activeSensors = new Map();

function makeSensorKey(clusterId, deviceId, channel) {
  return `${String(clusterId || "default")}|${String(deviceId || "")}|${Number(channel)}`;
}

// POST /api/sensor/alert
// Body: { cluster_id?, device_id?, channel, target, duration_seconds, reason }
// Creates or renews an Auto Hold session.  Only sends MQTT if this is
// a new session OR ?? minutes have passed since the last MQTT renew.
app.post("/api/sensor/alert", (req, res) => {
  const channel        = Number(req.body.channel);
  const target         = Number(req.body.target ?? 1);
  const durationSec    = Number(req.body.duration_seconds ?? 600);
  const reason         = String(req.body.reason || "sensor alert").slice(0, 63);
  const clusterId      = String(req.body.cluster_id || "default");
  const deviceId       = String(req.body.device_id || "");
  const nowMs          = Date.now();
  let   nowUnix        = Math.floor(nowMs / 1000);

  if (!channel || channel < 1 || channel > 16) {
    return res.status(400).json({ ok: false, error: "invalid_channel" });
  }
  if (target !== 0 && target !== 1) {
    return res.status(400).json({ ok: false, error: "invalid_target" });
  }
  if (durationSec < 1 || durationSec > 1800) {
    return res.status(400).json({ ok: false, error: "invalid_duration", max: 1800 });
  }

  // Resolve which device to target
  if (!cloudClusters.has(clusterId)) {
    return res.status(404).json({ ok: false, error: "cluster_not_found" });
  }

  // Try explicit node_id, then cloudDevices, then knownNodes
  let targetNodeId = Number(req.body.node_id) || 0;
  if (!targetNodeId && deviceId) {
    const registryKey = `${clusterId}|${deviceId}`;
    if (cloudDevices.has(registryKey)) {
      const dev = cloudDevices.get(registryKey);
      if (dev && dev.nodeId) targetNodeId = dev.nodeId;
    }
  }
  if (!targetNodeId) {
    // Pick any device in this cluster with a known nodeId
    for (const [, dev] of cloudDevices) {
      if (String(dev.clusterId || "default") === clusterId && dev.nodeId) {
        targetNodeId = dev.nodeId;
        break;
      }
    }
  }
  // Fallback: try knownNodes
  if (!targetNodeId) {
    for (const [nid, node] of knownNodes) {
      targetNodeId = Number(nid);
      break;
    }
  }
  if (!targetNodeId) {
    return res.status(503).json({ ok: false, error: "no_online_device", hint: "pass node_id explicitly" });
  }

  const key = makeSensorKey(clusterId, deviceId, channel);
  let sensor = activeSensors.get(key);
  const isNewSession = !sensor || sensor.session === 0;

  // Ensure command_ts is always increasing (MCU rejects older ts)
  if (sensor && sensor.lastCommandTs && nowUnix <= sensor.lastCommandTs) {
    nowUnix = sensor.lastCommandTs + 1;
  }
  if (sensor) sensor.lastCommandTs = nowUnix;

  if (isNewSession) {
    // First alert ??create new session
    sensor = {
      session:     crypto.randomBytes(4).readUInt32BE(0) >>> 0,  // positive uint32
      channel,
      target,
      durationSec,
      reason,
      clusterId,
      deviceId,
      targetNodeId,
      lastMqttRenewMs: 0,        // force immediate send
      lastAlertMs: nowMs,
      createdAtMs: nowMs,
    };
    activeSensors.set(key, sensor);
  } else {
    // Existing session ??update tracking, maybe renew
    sensor.lastAlertMs = nowMs;
    sensor.reason = reason;
    activeSensors.set(key, sensor);
  }

  // Decide whether to send MQTT now
  const elapsedSinceRenew = (nowMs - sensor.lastMqttRenewMs);
  const shouldSendMqtt = isNewSession || (elapsedSinceRenew >= 5 * 60 * 1000);

  let mqttResult = null;
  if (shouldSendMqtt) {
    const nodeInfo = resolveKnownNode(targetNodeId);
    const clusterName = nodeInfo?.cluster || clusterId;
    const payload = {
      channel,
      target,
      duration_seconds: durationSec,
      session: sensor.session,
      command_ts: nowUnix,
      reason,
      node_id: targetNodeId,
      request_id: crypto.randomBytes(8).toString("hex"),
    };
    const mqttOk = mqttPublishCommand(clusterName, "auto-hold", payload);
    mqttResult = mqttOk ? "sent" : "mqtt_unavailable";
    if (mqttOk) {
      sensor.lastMqttRenewMs = nowMs;
      sensor.targetNodeId = targetNodeId;
    }
  } else {
    mqttResult = "deferred";  // <5min since last MQTT, skip
  }

  return res.json({
    ok: true,
    session: sensor.session,
    channel,
    mqtt: mqttResult,
    is_new_session: isNewSession,
    next_renew_available_ms: sensor.lastMqttRenewMs + 5 * 60 * 1000,
  });
});

// POST /api/sensor/recover
// Body: { cluster_id?, device_id?, channel }
// Ends the active sensor Auto Hold for this channel.
app.post("/api/sensor/recover", (req, res) => {
  const channel   = Number(req.body.channel);
  const clusterId = String(req.body.cluster_id || "default");
  const deviceId  = String(req.body.device_id || "");
  const key       = makeSensorKey(clusterId, deviceId, channel);
  const sensor    = activeSensors.get(key);

  if (!sensor || sensor.session === 0) {
    return res.json({ ok: true, cleared: false, reason: "no_active_session" });
  }

  const nowUnix = Math.floor(Date.now() / 1000);
  const nodeInfo = resolveKnownNode(sensor.targetNodeId);
  const clusterName = nodeInfo?.cluster || clusterId;
  const payload = {
    channel,
    target: sensor.target,       // must match creation target
    duration_seconds: 0,         // clear
    session: sensor.session,
    command_ts: nowUnix,
    reason: "sensor_clear",
    node_id: sensor.targetNodeId,
    request_id: crypto.randomBytes(8).toString("hex"),
  };

  const mqttOk = mqttPublishCommand(clusterName, "auto-hold", payload);
  activeSensors.delete(key);

  return res.json({
    ok: true,
    cleared: true,
    session: sensor.session,
    channel,
    mqtt: mqttOk ? "sent" : "mqtt_unavailable",
  });
});

// GET /api/sensor/status ??list all active sensor sessions
app.get("/api/sensor/status", (_req, res) => {
  const list = [];
  for (const [key, s] of activeSensors) {
    list.push({
      key,
      channel: s.channel,
      target: s.target,
      session: s.session,
      duration_sec: s.durationSec,
      reason: s.reason,
      last_alert_ms: s.lastAlertMs,
      last_mqtt_renew_ms: s.lastMqttRenewMs,
      created_ms: s.createdAtMs,
    });
  }
  return res.json({ ok: true, active: list.length, sensors: list });
});

app.listen(HTTP_PORT, HTTP_HOST, () => {
  console.log(`[backend-test] http listening on ${HTTP_HOST}:${HTTP_PORT}`);
});

setInterval(syncKnownNodesFromBroker, 5000);
syncKnownNodesFromBroker();

setInterval(scanDuplicateMasters, DUPLICATE_MASTER_SCAN_MS);
setInterval(scanStaleKnownNodes, DUPLICATE_MASTER_SCAN_MS);

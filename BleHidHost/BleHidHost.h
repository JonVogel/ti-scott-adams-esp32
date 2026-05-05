/*
 * BleHidHost — multi-peer BLE HID host for ESP32-S3 (Bluedroid stack).
 *
 * Scans for HID devices, connects to up to MAX_PEERS of them
 * simultaneously, subscribes to input reports on each, and forwards
 * raw HID reports through a single user-supplied callback. The caller
 * is expected to dispatch on report shape (length / first byte) — keep
 * the keyboard parser and the gamepad parser as separate consumers.
 *
 * Why multi-peer: in a TI BASIC simulator you need both a keyboard
 * (to type programs) AND a gamepad (CALL JOYST). A single-peer host
 * forces the user to repeatedly forget/re-pair, which is unusable.
 *
 * Usage:
 *
 *   #include <BleHidHost.h>
 *
 *   void onHidReport(const uint8_t* data, size_t len) { ... }
 *
 *   void setup() {
 *     BleHidHost::setReportCallback(onHidReport);
 *     BleHidHost::begin("MyDeviceName");
 *   }
 *
 *   void loop() { BleHidHost::task(); }
 *
 * Pairing:
 *   BleHidHost::requestPairingMode()  — opens a 30s window during
 *     which any HID device that's NOT already paired gets adopted
 *     into the first free peer slot. Existing pairings are preserved.
 *
 *   BleHidHost::unpairAll()  — wipes all stored peers and disconnects.
 *     Use as a "factory reset" if a peer is replaced.
 *
 * State queries:
 *   BleHidHost::isConnected()   — true if ANY peer is connected
 *   BleHidHost::isReady()       — true if ANY peer has live notifications
 *   BleHidHost::inPairingMode() — true during the pairing window
 */

#pragma once

#include <Arduino.h>
#include <BLEDevice.h>
#include <BLESecurity.h>
#include <Preferences.h>

class BleHidHost
{
public:
  // 2 = keyboard + gamepad. Bluedroid on ESP32-S3 supports more
  // central connections, but 2 is what we need and keeps RAM tight.
  static const int MAX_PEERS = 2;

  typedef void (*ReportCallback)(const uint8_t* data, size_t len);

  static void setReportCallback(ReportCallback cb) { _cb = cb; }
  static void begin(const char* deviceName, const char* nvsNamespace = "blehidhost");
  static void task();
  static void enterPairingMode(uint32_t windowMs = 30000UL);
  static void unpairAll();

  // Safe to call from any context (BLE notify callbacks, ISRs, etc.).
  // The actual transition runs from task() on the main loop.
  static void requestPairingMode() { _pairingRequested = true; }
  static void requestUnpairAll()   { _unpairRequested  = true; }

  static bool isConnected();
  static bool isReady();
  static bool inPairingMode()  { return _pairingMode; }
  static int  peerCount();   // number of currently-connected peers

private:
  static BLEUUID _hidServiceUUID;
  static BLEUUID _reportCharUUID;

  struct Peer
  {
    BLEClient* client = nullptr;
    BLEAdvertisedDevice* target = nullptr;
    volatile bool connected = false;
    volatile bool ready = false;
    volatile bool doConnect = false;
    String savedAddress;
  };
  static Peer _peers[MAX_PEERS];

  static ReportCallback _cb;
  static volatile bool _doScan;
  static volatile bool _pairingMode;
  static volatile bool _pairingRequested;
  static volatile bool _unpairRequested;
  static unsigned long _pairingDeadline;
  static uint32_t _pairingWindowMs;

  static Preferences _prefs;
  static const char* _nvsNamespace;

  static const int _bootButtonPin = 0;

  // Internal helpers
  static void _notifyCb(BLERemoteCharacteristic* pChar,
                        uint8_t* data, size_t len, bool isNotify);
  static bool _connectToServer(int slot);
  static int  _findFreeSlot();
  static int  _findSlotByAddress(const String& addr);
  static bool _addressIsAlreadyKnownOrConnected(const String& addr);
  static void _persistPeer(int slot);
  static void _loadAllPeers();

  // Inner callback classes
  class ClientCallbacks;
  class ScanCallbacks;
};

// ---------------------------------------------------------------------------
// Static member definitions
// ---------------------------------------------------------------------------
inline BLEUUID BleHidHost::_hidServiceUUID((uint16_t)0x1812);
inline BLEUUID BleHidHost::_reportCharUUID((uint16_t)0x2A4D);

inline BleHidHost::Peer BleHidHost::_peers[BleHidHost::MAX_PEERS];

inline BleHidHost::ReportCallback BleHidHost::_cb = nullptr;
inline volatile bool BleHidHost::_doScan            = false;
inline volatile bool BleHidHost::_pairingMode       = false;
inline volatile bool BleHidHost::_pairingRequested  = false;
inline volatile bool BleHidHost::_unpairRequested   = false;
inline unsigned long BleHidHost::_pairingDeadline   = 0;
inline uint32_t      BleHidHost::_pairingWindowMs   = 30000UL;

inline Preferences BleHidHost::_prefs;
inline const char* BleHidHost::_nvsNamespace = "blehidhost";

// ---------------------------------------------------------------------------
// Slot-management helpers
// ---------------------------------------------------------------------------
inline int BleHidHost::_findFreeSlot()
{
  // First-pass: a slot is genuinely free (no connection, no saved
  // peer).
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (!_peers[i].connected && _peers[i].savedAddress.length() == 0)
    {
      return i;
    }
  }
  // Second-pass (only meaningful in pairing mode): every slot has a
  // saved address but is not connected. The user explicitly asked to
  // pair a NEW device, so evict the first non-connected slot —
  // forgetting one stale bond to make room. Without this, two stale
  // bonds permanently lock out fresh pairing.
  if (_pairingMode)
  {
    for (int i = 0; i < MAX_PEERS; i++)
    {
      if (!_peers[i].connected)
      {
        Serial.printf("BleHidHost: evicting stale bond on slot %d (%s) "
                      "to make room\n",
                      i, _peers[i].savedAddress.c_str());
        _peers[i].savedAddress = "";
        _peers[i].ready = false;
        _prefs.begin(_nvsNamespace, false);
        char key[16];
        snprintf(key, sizeof(key), "peer_addr_%d", i);
        _prefs.remove(key);
        _prefs.end();
        return i;
      }
    }
  }
  return -1;
}

inline int BleHidHost::_findSlotByAddress(const String& addr)
{
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].savedAddress.length() > 0 &&
        _peers[i].savedAddress.equalsIgnoreCase(addr))
    {
      return i;
    }
  }
  return -1;
}

inline bool BleHidHost::_addressIsAlreadyKnownOrConnected(const String& addr)
{
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].connected &&
        _peers[i].savedAddress.equalsIgnoreCase(addr))
    {
      return true;
    }
  }
  return false;
}

inline void BleHidHost::_persistPeer(int slot)
{
  _prefs.begin(_nvsNamespace, false);
  char key[16];
  snprintf(key, sizeof(key), "peer_addr_%d", slot);
  _prefs.putString(key, _peers[slot].savedAddress);
  _prefs.end();
}

inline void BleHidHost::_loadAllPeers()
{
  _prefs.begin(_nvsNamespace, true);

  // Load indexed keys "peer_addr_0", "peer_addr_1", ...
  for (int i = 0; i < MAX_PEERS; i++)
  {
    char key[16];
    snprintf(key, sizeof(key), "peer_addr_%d", i);
    _peers[i].savedAddress = _prefs.getString(key, "");
  }

  // Backward-compat: migrate the old single-peer "peer_addr" key into
  // slot 0 the first time this firmware runs.
  if (_peers[0].savedAddress.length() == 0)
  {
    String legacy = _prefs.getString("peer_addr", "");
    if (legacy.length() > 0)
    {
      _peers[0].savedAddress = legacy;
      _prefs.end();
      _persistPeer(0);
      _prefs.begin(_nvsNamespace, false);
      _prefs.remove("peer_addr");
    }
  }
  _prefs.end();

  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].savedAddress.length() > 0)
    {
      Serial.printf("BleHidHost: peer %d = %s\n",
                    i, _peers[i].savedAddress.c_str());
    }
  }
}

// ---------------------------------------------------------------------------
// State queries
// ---------------------------------------------------------------------------
inline bool BleHidHost::isConnected()
{
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].connected) return true;
  }
  return false;
}

inline bool BleHidHost::isReady()
{
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].ready) return true;
  }
  return false;
}

inline int BleHidHost::peerCount()
{
  int n = 0;
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].connected) n++;
  }
  return n;
}

// ---------------------------------------------------------------------------
// Inner callback classes
// ---------------------------------------------------------------------------
class BleHidHost::ClientCallbacks : public BLEClientCallbacks
{
public:
  ClientCallbacks(int peerIdx) : _peerIdx(peerIdx) {}

private:
  int _peerIdx;

  void onConnect(BLEClient* client) override
  {
    Serial.printf("BleHidHost: peer %d connected.\n", _peerIdx);
    _peers[_peerIdx].connected = true;
  }
  void onDisconnect(BLEClient* client) override
  {
    Serial.printf("BleHidHost: peer %d disconnected.\n", _peerIdx);
    _peers[_peerIdx].connected = false;
    _peers[_peerIdx].ready = false;
    _doScan = true;   // try to reconnect this peer
  }
};

class BleHidHost::ScanCallbacks : public BLEAdvertisedDeviceCallbacks
{
  void onResult(BLEAdvertisedDevice ad) override
  {
    String addr = ad.getAddress().toString();
    String name = ad.getName().c_str();
    bool isHid = ad.haveServiceUUID() &&
                 ad.isAdvertisingService(_hidServiceUUID);

    // Some BLE-HID gamepads (8BitDo Zero 2, several generic pads)
    // don't include the HID Service UUID 0x1812 in their primary
    // advertisement — they put it in the GATT services after
    // connection. Use a name-substring fallback so those still
    // pair. Names checked are common gamepad/keyboard prefixes.
    bool nameLooksHid = false;
    if (name.length() > 0)
    {
      const char* hits[] = {
        "8Bit", "8bit", "Zero", "SN30", "Gamepad", "gamepad",
        "Joystick", "joystick", "Pro Controller", "DualSense",
        "DualShock", "Xbox", "Keyboard", "keyboard", "Mouse",
        // 8BitDo internal model codes that appear instead of the
        // friendly name on BLE: Q35 = Zero 2.
        "Q35"
      };
      for (auto h : hits)
      {
        if (name.indexOf(h) >= 0) { nameLooksHid = true; break; }
      }
    }

    // Scan-spam debug — enable when troubleshooting which devices
    // are visible during pairing.
#if 0
    if (_pairingMode)
    {
      Serial.printf("BleHidHost: scan saw %s name='%s' hid=%d hint=%d\n",
                    addr.c_str(), name.c_str(),
                    isHid ? 1 : 0, nameLooksHid ? 1 : 0);
    }
#endif

    // Already connected as one of our peers? Ignore.
    if (_addressIsAlreadyKnownOrConnected(addr)) return;

    // Decide which slot, if any, this advertisement should bind to:
    //  1. A saved-but-not-connected peer slot whose address matches.
    //  2. (Only in pairing mode + isHid) the first totally free slot.
    int slot = _findSlotByAddress(addr);
    if (slot >= 0 && _peers[slot].connected)
    {
      // Already connected on that slot — shouldn't happen given the
      // check above, but bail anyway.
      return;
    }
    if (slot < 0)
    {
      // In pairing mode, accept devices that EITHER advertise the HID
      // service in primary advertising (TI-faithful), OR have a name
      // that looks like a HID device. The latter catches 8BitDo
      // gamepads etc. that put HID in the GATT only — _connectToServer
      // probes for the HID service post-connect and disconnects if
      // it's missing, so a name-based false positive is recoverable.
      if (!_pairingMode || (!isHid && !nameLooksHid)) return;
      slot = _findFreeSlot();
      if (slot < 0)
      {
        Serial.printf("BleHidHost: pairing — no free slot for %s\n",
                      addr.c_str());
        return;
      }
    }

    Serial.printf("BleHidHost: %s %s (%s) → slot %d\n",
                  _peers[slot].savedAddress.length() ? "Reconnecting" : "Pairing",
                  ad.getName().c_str(), addr.c_str(), slot);

    BLEDevice::getScan()->stop();
    if (_peers[slot].target != nullptr) delete _peers[slot].target;
    _peers[slot].target = new BLEAdvertisedDevice(ad);
    _peers[slot].doConnect = true;
  }
};

// ---------------------------------------------------------------------------
// Implementation
// ---------------------------------------------------------------------------
inline void BleHidHost::_notifyCb(BLERemoteCharacteristic* pChar,
                                   uint8_t* data, size_t len, bool isNotify)
{
  // Single shared callback. Caller dispatches by report shape.
  if (_cb != nullptr) _cb(data, len);
}

inline bool BleHidHost::_connectToServer(int slot)
{
  Peer& p = _peers[slot];
  if (p.target == nullptr) return false;

  Serial.printf("BleHidHost: slot %d connecting to %s (%s)...\n",
                slot, p.target->getName().c_str(),
                p.target->getAddress().toString().c_str());

  if (p.client != nullptr)
  {
    // Stale client from a previous attempt — release it.
    if (p.client->isConnected()) p.client->disconnect();
    delete p.client;
    p.client = nullptr;
  }

  p.client = BLEDevice::createClient();
  p.client->setClientCallbacks(new ClientCallbacks(slot));

  // Some BLE-HID peripherals (8BitDo gamepads in particular) reject
  // the first connect attempt after entering pairing mode — usually
  // a timing race between our scan-stop and their advertising
  // window. Retry up to 3 times before giving up.
  bool ok = false;
  for (int attempt = 0; attempt < 3; attempt++)
  {
    if (p.client->connect(p.target))
    {
      ok = true;
      break;
    }
    Serial.printf("BleHidHost: slot %d connect() attempt %d failed%s\n",
                  slot, attempt + 1,
                  attempt < 2 ? ", retrying..." : ".");
    if (p.client->isConnected()) p.client->disconnect();
    delay(200);
  }
  if (!ok) return false;
  p.client->setMTU(185);

  BLERemoteService* pHidService = p.client->getService(_hidServiceUUID);
  if (pHidService == nullptr)
  {
    Serial.printf("BleHidHost: slot %d HID service not found.\n", slot);
    p.client->disconnect();
    return false;
  }

  int subscribed = 0;
  std::map<std::string, BLERemoteCharacteristic*>* pCharMap =
    pHidService->getCharacteristics();

  for (auto const& entry : *pCharMap)
  {
    BLERemoteCharacteristic* pChar = entry.second;
    if (pChar->getUUID().equals(_reportCharUUID) && pChar->canNotify())
    {
      BLERemoteDescriptor* pReportRef =
        pChar->getDescriptor(BLEUUID((uint16_t)0x2908));
      if (pReportRef != nullptr)
      {
        String refValue = pReportRef->readValue();
        if (refValue.length() >= 2 && refValue[1] == 1)   // input report
        {
          pChar->registerForNotify(_notifyCb);
          subscribed++;
        }
      }
      else
      {
        pChar->registerForNotify(_notifyCb);
        subscribed++;
      }
    }
  }

  if (subscribed == 0)
  {
    Serial.printf("BleHidHost: slot %d no input reports found.\n", slot);
    p.client->disconnect();
    return false;
  }

  p.ready = true;
  Serial.printf("BleHidHost: slot %d ready, %d input report(s).\n",
                slot, subscribed);

  // Persist this peer's address (it might be a new pairing).
  String connectedAddr = p.target->getAddress().toString();
  if (connectedAddr != p.savedAddress)
  {
    p.savedAddress = connectedAddr;
    _persistPeer(slot);
    Serial.printf("BleHidHost: slot %d saved %s\n", slot,
                  p.savedAddress.c_str());
  }
  return true;
}

inline void BleHidHost::begin(const char* deviceName, const char* nvsNamespace)
{
  _nvsNamespace = nvsNamespace;

  _loadAllPeers();

  BLEDevice::init(deviceName);
  BLESecurity* pSecurity = new BLESecurity();
  pSecurity->setCapability(ESP_IO_CAP_NONE);
  pSecurity->setAuthenticationMode(true, false, true);
  // Default security callbacks — required on some boards for bonding
  // to complete before service discovery, otherwise HID keyboards like
  // the L75 expose only generic services on first connect.
  BLEDevice::setSecurityCallbacks(new BLESecurityCallbacks());

  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  pScan->setInterval(1349);
  pScan->setWindow(449);
  pScan->setActiveScan(true);
  pScan->start(5, false);
  _doScan = true;

  pinMode(_bootButtonPin, INPUT_PULLUP);
  Serial.println("BleHidHost: scanning...");
}

inline void BleHidHost::enterPairingMode(uint32_t windowMs)
{
  Serial.printf("BleHidHost: entering pairing mode (%lus). "
                "Existing peers preserved.\n", (unsigned long)(windowMs / 1000));
  _pairingMode = true;
  _pairingWindowMs = windowMs;
  _pairingDeadline = millis() + windowMs;

  BLEScan* pScan = BLEDevice::getScan();
  pScan->stop();
  pScan->clearResults();
  _doScan = true;
}

inline void BleHidHost::unpairAll()
{
  Serial.println("BleHidHost: forgetting all peers.");
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].client != nullptr && _peers[i].client->isConnected())
    {
      _peers[i].client->disconnect();
    }
    _peers[i].savedAddress = "";
    _peers[i].connected = false;
    _peers[i].ready = false;
  }
  _prefs.begin(_nvsNamespace, false);
  for (int i = 0; i < MAX_PEERS; i++)
  {
    char key[16];
    snprintf(key, sizeof(key), "peer_addr_%d", i);
    _prefs.remove(key);
  }
  _prefs.remove("peer_addr");   // legacy
  _prefs.end();
}

inline void BleHidHost::task()
{
  if (_pairingRequested)
  {
    _pairingRequested = false;
    enterPairingMode(_pairingWindowMs);
  }
  if (_unpairRequested)
  {
    _unpairRequested = false;
    unpairAll();
  }

  // Process any pending connect requests.
  for (int i = 0; i < MAX_PEERS; i++)
  {
    if (_peers[i].doConnect)
    {
      _peers[i].doConnect = false;
      _connectToServer(i);
    }
  }

  if (_pairingMode && (long)(millis() - _pairingDeadline) >= 0)
  {
    Serial.println("BleHidHost: pairing window expired.");
    _pairingMode = false;
  }

  // Only scan while in pairing mode. Previously we also scanned
  // continuously whenever any saved peer was missing — but a single
  // unreachable saved peer (e.g. a keyboard that's been turned off
  // for good) leaves the radio in perpetual active-scan mode, which
  // bogs down the whole sketch. The watchdog in ble_keyboard.h
  // detects "no peer connected for 5 s" and asks for pairing mode,
  // so transient disconnects still auto-recover.
  if (_pairingMode && _doScan)
  {
    _doScan = false;
    BLEScan* pScan = BLEDevice::getScan();
    pScan->clearResults();
    pScan->start(5, false);
    _doScan = true;
  }

  // BOOT button → request pairing.
  if (digitalRead(_bootButtonPin) == LOW)
  {
    delay(50);
    if (digitalRead(_bootButtonPin) == LOW)
    {
      enterPairingMode(_pairingWindowMs);
      while (digitalRead(_bootButtonPin) == LOW) { delay(50); }
    }
  }
}

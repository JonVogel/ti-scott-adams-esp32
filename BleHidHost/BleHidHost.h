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
  // User-initiated pairing — opens the window AND flags it so the
  // application shows a takeover UI. For silent watchdog
  // reconnections, use requestSilentScan() instead.
  static void requestPairingMode() { _userInitiatedRequested = true; _pairingRequested = true; }
  static void requestUnpairAll()   { _unpairRequested  = true; }

  static bool isConnected();
  static bool isReady();
  static bool inPairingMode()  { return _pairingMode; }
  // True only when the user explicitly asked for pairing (BOOT
  // button / F12), not when a silent watchdog reconnect kicked off a
  // scan window. Lets the application show a full-screen pairing UI
  // for user-initiated requests while keeping background reconnects
  // invisible.
  static bool userInitiatedPairing() { return _pairingMode && _userInitiatedPairing; }
  // Request a *silent* scan window — same machinery, but the
  // userInitiatedPairing() flag stays false so the application can
  // skip any UI takeover.
  static void requestSilentScan() { _pairingRequested = true; }
  // Milliseconds remaining in the current pairing window (0 if not
  // pairing).
  static unsigned long pairingRemainingMs()
  {
    if (!_pairingMode) return 0;
    long remain = (long)(_pairingDeadline - millis());
    return remain > 0 ? (unsigned long)remain : 0;
  }
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
  static volatile bool _userInitiatedRequested;
  static volatile bool _userInitiatedPairing;
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
  static void _bootButtonIsr();

  // Inner callback classes
  class ClientCallbacks;
  class ScanCallbacks;
  class SecurityCallbacks;
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
inline volatile bool BleHidHost::_pairingRequested      = false;
inline volatile bool BleHidHost::_userInitiatedRequested = false;
inline volatile bool BleHidHost::_userInitiatedPairing  = false;
inline volatile bool BleHidHost::_unpairRequested       = false;
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
        // Apple keyboards: "Magic Keyboard" matches "Keyboard"
        // already, but legacy "Apple Wireless Keyboard" likewise
        // does — adding bare "Magic" / "Apple" / "iPad" / "iMac"
        // covers any short-form names we haven't seen.
        "Magic", "Apple", "iPad", "iMac",
        // Third-party BLE keyboards that advertise with a vendor /
        // model name and no HID UUID in the primary packet.
        "OMOTON", "Omoton", "KB066",
        // 8BitDo internal model codes that appear instead of the
        // friendly name on BLE: Q35 = Zero 2.
        "Q35"
      };
      for (auto h : hits)
      {
        if (name.indexOf(h) >= 0) { nameLooksHid = true; break; }
      }
    }

    // Scan-spam debug. On while we're chasing an Apple keyboard that
    // isn't being detected — flip back to #if 0 once the issue is
    // pinned down so we don't fill the serial monitor every pairing
    // window. Includes service UUIDs and manufacturer ID so we can
    // identify HID devices that advertise with an empty name.
#if 1
    if (_pairingMode)
    {
      int uuidCount = ad.getServiceUUIDCount();
      char uuids[120] = {0};
      int upos = 0;
      for (int i = 0; i < uuidCount && upos < (int)sizeof(uuids) - 8; i++)
      {
        String s = ad.getServiceUUID(i).toString().c_str();
        upos += snprintf(uuids + upos, sizeof(uuids) - upos,
                         "%s%s", upos ? "," : "", s.c_str());
      }
      uint16_t mfg = 0;
      if (ad.haveManufacturerData())
      {
        String md = ad.getManufacturerData().c_str();
        if (md.length() >= 2)
        {
          mfg = ((uint8_t)md[1] << 8) | (uint8_t)md[0];
        }
      }
      Serial.printf("BleHidHost: scan saw %s name='%s' hid=%d hint=%d uuids=%d[%s] mfg=0x%04x\n",
                    addr.c_str(), name.c_str(),
                    isHid ? 1 : 0, nameLooksHid ? 1 : 0,
                    uuidCount, uuids, mfg);
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

// Pairing security callbacks. We declare ESP_IO_CAP_OUT (host can
// display) so that keyboards which insist on Passkey-Entry pairing
// (OMOTON KB066, certain Apple keyboards, anything that wants real
// MITM protection) get a 6-digit code generated for them. The user
// sees the code on serial / screen and types it on the keyboard.
//
// For "Just Works" peripherals (8BitDo Zero 2, ProtoArc L75 in its
// default mode, etc.) the negotiation falls back to no-passkey since
// they don't request MITM — so existing devices keep pairing the
// same way they always have.
class BleHidHost::SecurityCallbacks : public BLESecurityCallbacks
{
  uint32_t onPassKeyRequest() override
  {
    // Peer wants us to enter a passkey it's displaying. Our hosts
    // are headless from the BLE-stack's view, so this case shouldn't
    // arise with our IO_CAP_OUT setting. Return 0 if it does.
    Serial.println("BleHidHost: peer requested passkey input — not supported.");
    return 0;
  }

  void onPassKeyNotify(uint32_t pass_key) override
  {
    Serial.printf("\n"
                  "==========================================\n"
                  "BleHidHost: PAIRING PASSKEY  %06lu\n"
                  "Type those 6 digits on the keyboard then\n"
                  "press Enter to complete pairing.\n"
                  "==========================================\n",
                  (unsigned long)pass_key);
  }

  bool onSecurityRequest() override { return true; }

  bool onConfirmPIN(uint32_t pin) override
  {
    Serial.printf("BleHidHost: confirm PIN %06lu (auto-accepting).\n",
                  (unsigned long)pin);
    return true;
  }

  // onAuthenticationComplete deliberately not overridden — its
  // signature references esp_ble_auth_cmpl_t which lives in
  // esp_gap_ble_api.h, and that header's path is awkward to add to
  // an Arduino library. The base class's default implementation
  // already prints success/failure to Serial on Bluedroid.
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
  // ESP_IO_CAP_OUT lets us do Passkey Entry when the peer requires
  // it (OMOTON-style keyboards). For peers that don't request MITM,
  // pairing falls back to Just Works as before.
  pSecurity->setCapability(ESP_IO_CAP_OUT);
  pSecurity->setAuthenticationMode(true, false, true);
  // Custom security callbacks: print any generated passkey to Serial
  // so the user can type it on the keyboard.
  BLEDevice::setSecurityCallbacks(new SecurityCallbacks());

  BLEScan* pScan = BLEDevice::getScan();
  pScan->setAdvertisedDeviceCallbacks(new ScanCallbacks());
  // 100% duty cycle (window == interval). The previous 33% was
  // missing peripherals whose advertising interval didn't line up
  // with our listen windows — observed with an OMOTON KB066. Since
  // our scans only run during the pairing window (e0e2898 fix), the
  // higher duty doesn't burn radio time when idle.
  pScan->setInterval(160);   // 100 ms (units of 0.625 ms)
  pScan->setWindow(160);     // 100 ms = continuous listen
  // Passive scan: just listen, never send scan requests. Some
  // peripherals (OMOTON KB066 observed) won't deliver primary
  // adv packets reliably to scanners that probe them with active
  // requests — even though Windows / iPhone see them fine. If we
  // need scan-response data later we can flip this back, but it
  // costs us nothing for keyboards that put HID UUID or name
  // directly in the primary advertisement.
  pScan->setActiveScan(false);
  pScan->start(5, false);
  _doScan = true;

  pinMode(_bootButtonPin, INPUT_PULLUP);
  // Attach falling-edge ISR so a BOOT-button press anywhere — even
  // when the main loop is held up by something else — sets the
  // pairing-request flag immediately. The actual BLE work runs in
  // the next task() call (BLE stack APIs aren't ISR-safe).
  attachInterrupt(digitalPinToInterrupt(_bootButtonPin),
                  _bootButtonIsr, FALLING);
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
    // Latch user-initiated flag at the time the window opens. Both
    // BOOT-button-ISR and F12 set _userInitiatedRequested; the
    // watchdog clears it before requesting via requestSilentScan()
    // (which only sets _pairingRequested).
    _userInitiatedPairing = _userInitiatedRequested;
    _userInitiatedRequested = false;
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
    _userInitiatedPairing = false;
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

  // BOOT button is now ISR-driven — _bootButtonIsr sets
  // _pairingRequested directly. Nothing to poll here.
}

// ISR for the BOOT button. Just flag the request — actual BLE work
// happens on the next task() call. Not IRAM_ATTR because the volatile
// flag we touch lives in flash; the linker rejects mixed IRAM/flash
// access. Acceptable since BOOT-press timing isn't critical and we
// won't be in the middle of a flash write when the user prods it.
inline void BleHidHost::_bootButtonIsr()
{
  // Idempotent flag set; bounce-safe because re-asserting it has no
  // effect.
  _pairingRequested = true;
}

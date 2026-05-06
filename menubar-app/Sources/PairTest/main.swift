// PairTest — bootstrap-pair our device's NUS service before macOS HID bonds it.
//
// Device advertises HID (0x1812) primary, but the NUS service IS present as a
// secondary GATT service after connect. Our firmware just couldn't fit NUS UUID
// into the legacy 31-byte adv packet alongside HID + battery + name. So we:
//
//   1. Scan unfiltered, match by name "ClaudeApprover".
//   2. Connect.
//   3. Discover NUS service post-connect.
//   4. Save peripheral identifier to ~/.claude-approver/peripheral.uuid
//   5. Subscribe to TX, send {"cmd":"status"} via RX.
//
// Subsequent runs use the saved identifier with retrievePeripherals(withIdentifiers:),
// which is the path that should keep working after macOS HID-bonds the same
// device.

import Foundation
import CoreBluetooth

setbuf(stdout, nil)
setbuf(stderr, nil)

let DEVICE_NAME = "ClaudeApprover"
let NUS_SERVICE = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
let NUS_RX_CHAR = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")  // host -> device
let NUS_TX_CHAR = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")  // device -> host

let stateDir = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent(".claude-approver", isDirectory: true)
let uuidFile = stateDir.appendingPathComponent("peripheral.uuid")

func savePeripheralUUID(_ uuid: UUID) {
    try? FileManager.default.createDirectory(at: stateDir, withIntermediateDirectories: true)
    try? uuid.uuidString.write(to: uuidFile, atomically: true, encoding: .utf8)
    print("[PairTest] saved peripheral UUID -> \(uuidFile.path)")
}

func loadPeripheralUUID() -> UUID? {
    guard let s = try? String(contentsOf: uuidFile, encoding: .utf8) else { return nil }
    return UUID(uuidString: s.trimmingCharacters(in: .whitespacesAndNewlines))
}

class Buddy: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var central: CBCentralManager!
    var peripheral: CBPeripheral?
    var rxChar: CBCharacteristic?
    var triedSavedUUID = false
    var connectedOnce = false

    func start() {
        central = CBCentralManager(delegate: self, queue: nil)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            print("[PairTest] poweredOn")
            tryConnectFromSaved()
            if peripheral == nil {
                print("[PairTest] starting unfiltered scan, matching by name '\(DEVICE_NAME)'")
                central.scanForPeripherals(withServices: nil,
                    options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
            }
        case .unauthorized:
            print("[PairTest] Bluetooth permission DENIED")
            print("[PairTest] System Settings → Privacy & Security → Bluetooth → grant Terminal")
            exit(2)
        case .poweredOff:
            print("[PairTest] Bluetooth OFF")
            exit(2)
        default:
            print("[PairTest] state \(central.state.rawValue)")
        }
    }

    func tryConnectFromSaved() {
        guard let saved = loadPeripheralUUID() else { return }
        print("[PairTest] saved UUID: \(saved)")
        let peripherals = central.retrievePeripherals(withIdentifiers: [saved])
        if let p = peripherals.first {
            print("[PairTest] retrievePeripherals returned \(p.name ?? "?")")
            peripheral = p
            p.delegate = self
            triedSavedUUID = true
            central.connect(p, options: nil)
        } else {
            print("[PairTest] retrievePeripherals returned nothing for saved UUID")
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = peripheral.name
            ?? advertisementData[CBAdvertisementDataLocalNameKey] as? String
            ?? ""
        if name != DEVICE_NAME { return }
        print("[PairTest] discovered \(name) rssi=\(RSSI) id=\(peripheral.identifier)")
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        savePeripheralUUID(peripheral.identifier)
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectedOnce = true
        print("[PairTest] connected — discovering services...")
        peripheral.discoverServices(nil)  // discover all so we surface NUS even if not advertised
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral, error: Error?) {
        print("[PairTest] connect failed: \(error?.localizedDescription ?? "?")")
        if triedSavedUUID && !connectedOnce {
            print("[PairTest] saved UUID didn't connect — falling back to scan")
            triedSavedUUID = false
            self.peripheral = nil
            central.scanForPeripherals(withServices: nil, options: nil)
        } else {
            exit(1)
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        print("[PairTest] services: \(services.map { $0.uuid.uuidString })")
        var foundNUS = false
        for svc in services {
            if svc.uuid == NUS_SERVICE {
                foundNUS = true
                peripheral.discoverCharacteristics([NUS_RX_CHAR, NUS_TX_CHAR], for: svc)
            }
        }
        if !foundNUS {
            print("[PairTest] NUS service not present on this peripheral")
            exit(1)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let chars = service.characteristics else { return }
        for c in chars {
            if c.uuid == NUS_RX_CHAR { rxChar = c }
            if c.uuid == NUS_TX_CHAR {
                peripheral.setNotifyValue(true, for: c)
            }
        }
        print("[PairTest] subscribed to TX")
        if let rx = rxChar {
            let hello = "{\"cmd\":\"status\"}\n".data(using: .utf8)!
            peripheral.writeValue(hello, for: rx, type: .withResponse)
            print("[PairTest] sent {\"cmd\":\"status\"}")
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if let data = characteristic.value, let s = String(data: data, encoding: .utf8) {
            print("[PairTest TX] \(s.trimmingCharacters(in: .whitespacesAndNewlines))")
        }
    }
}

let b = Buddy()
b.start()
RunLoop.main.run()

// BLEController — owns the CoreBluetooth central, scans for the device on
// first launch, persists the peripheral identifier, and reconnects on every
// subsequent launch via retrievePeripherals(withIdentifiers:). Parses incoming
// newline-delimited JSON lines from the NUS TX characteristic.

import Foundation
import CoreBluetooth
import Combine

private let DEVICE_NAME   = "ClaudeApprover"
private let NUS_SERVICE   = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
private let NUS_RX_CHAR   = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
private let NUS_TX_CHAR   = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

private let stateDir = FileManager.default.homeDirectoryForCurrentUser
    .appendingPathComponent(".claude-approver", isDirectory: true)
private let uuidFile = stateDir.appendingPathComponent("peripheral.uuid")

enum BLEStatus: Equatable {
    case poweredOff
    case unauthorized
    case scanning
    case connecting
    case connected
    case disconnected
    case unknown
}

final class BLEController: NSObject, ObservableObject {
    @Published private(set) var status: BLEStatus = .unknown
    @Published private(set) var lastIncomingLine: String = ""
    @Published private(set) var deviceIdentifier: UUID?

    /// Hook called on every parsed JSON event from the device.
    var onEvent: (([String: Any]) -> Void)?

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var rxChar: CBCharacteristic?
    private var txChar: CBCharacteristic?
    private var rxBuffer = Data()
    private var triedSavedUUID = false

    func start() {
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func send(_ json: [String: Any]) {
        guard let rx = rxChar, let p = peripheral, p.state == .connected else {
            return
        }
        guard
            let data = try? JSONSerialization.data(withJSONObject: json),
            var line = String(data: data, encoding: .utf8)
        else { return }
        line += "\n"
        if let payload = line.data(using: .utf8) {
            p.writeValue(payload, for: rx, type: .withResponse)
        }
    }

    func reconnect() {
        if let p = peripheral, p.state == .connected {
            central.cancelPeripheralConnection(p)
        } else {
            tryConnectFromSaved()
        }
    }

    func forgetSavedDevice() {
        try? FileManager.default.removeItem(at: uuidFile)
        deviceIdentifier = nil
    }

    // MARK: - Internal

    private func tryConnectFromSaved() {
        guard let saved = loadSavedUUID() else {
            startScan()
            return
        }
        deviceIdentifier = saved
        let knownPeripherals = central.retrievePeripherals(withIdentifiers: [saved])
        if let p = knownPeripherals.first {
            peripheral = p
            p.delegate = self
            triedSavedUUID = true
            status = .connecting
            central.connect(p, options: nil)
        } else {
            startScan()
        }
    }

    private func startScan() {
        guard central.state == .poweredOn else { return }
        status = .scanning
        central.scanForPeripherals(withServices: nil,
            options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
    }

    private func loadSavedUUID() -> UUID? {
        guard let s = try? String(contentsOf: uuidFile, encoding: .utf8) else {
            return nil
        }
        return UUID(uuidString: s.trimmingCharacters(in: .whitespacesAndNewlines))
    }

    private func saveUUID(_ uuid: UUID) {
        try? FileManager.default.createDirectory(at: stateDir, withIntermediateDirectories: true)
        try? uuid.uuidString.write(to: uuidFile, atomically: true, encoding: .utf8)
        deviceIdentifier = uuid
    }
}

// MARK: - CBCentralManagerDelegate

extension BLEController: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn:
            status = .disconnected
            tryConnectFromSaved()
        case .poweredOff:        status = .poweredOff
        case .unauthorized:      status = .unauthorized
        default:                 status = .unknown
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didDiscover peripheral: CBPeripheral,
                        advertisementData: [String : Any], rssi RSSI: NSNumber) {
        let name = peripheral.name
            ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String)
            ?? ""
        guard name == DEVICE_NAME else { return }
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        saveUUID(peripheral.identifier)
        status = .connecting
        central.connect(peripheral, options: nil)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        rxBuffer.removeAll(keepingCapacity: true)
        peripheral.discoverServices(nil)
    }

    func centralManager(_ central: CBCentralManager,
                        didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        rxChar = nil
        txChar = nil
        status = .disconnected
        // Auto-reconnect via saved identifier with a short backoff.
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
            self?.tryConnectFromSaved()
        }
    }

    func centralManager(_ central: CBCentralManager,
                        didFailToConnect peripheral: CBPeripheral, error: Error?) {
        if triedSavedUUID {
            triedSavedUUID = false
            self.peripheral = nil
            startScan()
        } else {
            status = .disconnected
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) { [weak self] in
                self?.tryConnectFromSaved()
            }
        }
    }
}

// MARK: - CBPeripheralDelegate

extension BLEController: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        for svc in services where svc.uuid == NUS_SERVICE {
            peripheral.discoverCharacteristics([NUS_RX_CHAR, NUS_TX_CHAR], for: svc)
        }
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let chars = service.characteristics else { return }
        for c in chars {
            if c.uuid == NUS_RX_CHAR { rxChar = c }
            if c.uuid == NUS_TX_CHAR {
                txChar = c
                peripheral.setNotifyValue(true, for: c)
            }
        }
        status = .connected
    }

    func peripheral(_ peripheral: CBPeripheral,
                    didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard let data = characteristic.value else { return }
        rxBuffer.append(data)
        // Each newline-terminated chunk is one JSON object.
        while let nlIndex = rxBuffer.firstIndex(of: 0x0A) {
            let lineData = rxBuffer.prefix(nlIndex)
            rxBuffer.removeSubrange(0...nlIndex)
            if let line = String(data: lineData, encoding: .utf8)?
                .trimmingCharacters(in: .whitespacesAndNewlines),
               !line.isEmpty {
                lastIncomingLine = line
                handleLine(line)
            }
        }
    }

    private func handleLine(_ line: String) {
        guard
            let data = line.data(using: .utf8),
            let any = try? JSONSerialization.jsonObject(with: data),
            let obj = any as? [String: Any]
        else { return }
        onEvent?(obj)
    }
}

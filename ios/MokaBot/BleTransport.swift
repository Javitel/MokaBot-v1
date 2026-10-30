import CoreBluetooth
import Foundation

/// The MokaBot's Bluetooth LE service, see the firmware's src/logic/bleLink.hpp.
/// Messages are split into packets of at most the write length; the first
/// byte of each packet flags the last one.
///   request:  "<METHOD> <path>[?query]\n<body>"
///   response: "<status> <content type>\n<body>"
enum MokaBle {
    static let service = CBUUID(string: "6d6f6b61-626f-7400-0000-000000000001")
    static let rx = CBUUID(string: "6d6f6b61-626f-7400-0000-000000000002")
    static let tx = CBUUID(string: "6d6f6b61-626f-7400-0000-000000000003")
    static let lastPacket: UInt8 = 0x01
}

/// A MokaBot seen in a Bluetooth scan: `name` is its device name, `rssi` its signal (dBm).
struct FoundDevice: Identifiable {
    let peripheral: CBPeripheral
    let name: String
    let rssi: Int
    var id: UUID { peripheral.identifier }
}

/// The one CoreBluetooth central of the app: scans and connects. Its
/// callbacks arrive on the main queue; the async methods run there too.
final class BleCentral: NSObject, CBCentralManagerDelegate {
    static let shared = BleCentral()

    private var manager: CBCentralManager!
    private var stateWaiters: [CheckedContinuation<Void, Error>] = []
    private var found: [UUID: FoundDevice] = [:]
    private var stopOn: UUID?
    private var scanDone: CheckedContinuation<Void, Never>?
    private var connectWaiters: [UUID: CheckedContinuation<Void, Error>] = [:]
    private var transports: [UUID: BleTransport] = [:]

    private override init() {
        super.init()
        manager = CBCentralManager(delegate: self, queue: nil)
    }

    private var stateError: TransportError {
        switch manager.state {
        case .poweredOff: return TransportError(message: "Switch Bluetooth on first.")
        case .unauthorized: return TransportError(message: "Bluetooth permission denied (Settings > MokaBot).")
        case .unsupported: return TransportError(message: "This device has no Bluetooth LE.")
        default: return TransportError(message: "Bluetooth is not available.")
        }
    }

    /// Waits until Bluetooth is on; throws when it is off or not allowed.
    @MainActor
    private func ready() async throws {
        switch manager.state {
        case .poweredOn:
            return
        case .unknown, .resetting:
            try await withCheckedThrowingContinuation { stateWaiters.append($0) }
        default:
            throw stateError
        }
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .unknown || central.state == .resetting { return }
        let waiters = stateWaiters
        stateWaiters = []
        for waiter in waiters {
            if central.state == .poweredOn { waiter.resume() } else { waiter.resume(throwing: stateError) }
        }
    }

    /// Scans for MokaBots for `duration` seconds, strongest signal first.
    /// Stops early when the device `stopOn` (the last one used) is seen.
    @MainActor
    func scan(duration: TimeInterval = 3, stopOn: UUID? = nil) async throws -> [FoundDevice] {
        try await ready()
        found = [:]
        self.stopOn = stopOn
        // Duplicates: the name arrives in the scan response, after the advertisement.
        manager.scanForPeripherals(withServices: [MokaBle.service],
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            scanDone = continuation
            DispatchQueue.main.asyncAfter(deadline: .now() + duration) { [weak self] in self?.finishScan() }
        }
        return found.values.sorted { $0.rssi > $1.rssi }
    }

    private func finishScan() {
        manager.stopScan()
        let continuation = scanDone
        scanDone = nil
        continuation?.resume()
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = advertisementData[CBAdvertisementDataLocalNameKey] as? String
            ?? found[peripheral.identifier]?.name ?? peripheral.name ?? "MokaBot"
        found[peripheral.identifier] = FoundDevice(peripheral: peripheral, name: name, rssi: RSSI.intValue)
        if peripheral.identifier == stopOn { finishScan() }
    }

    @MainActor
    func connect(_ device: FoundDevice) async throws -> BleTransport {
        try await ready()
        let peripheral = device.peripheral
        let transport = BleTransport(peripheral: peripheral, name: device.name)
        transports[peripheral.identifier] = transport
        do {
            try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
                connectWaiters[peripheral.identifier] = continuation
                manager.connect(peripheral, options: nil)
                DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self] in
                    guard let self = self, let waiter = self.connectWaiters.removeValue(forKey: peripheral.identifier) else { return }
                    self.manager.cancelPeripheralConnection(peripheral)
                    waiter.resume(throwing: TransportError(message: "timed out"))
                }
            }
            try await transport.setUp()
        } catch {
            transports[peripheral.identifier] = nil
            manager.cancelPeripheralConnection(peripheral)
            throw error
        }
        return transport
    }

    func disconnect(_ peripheral: CBPeripheral) {
        manager.cancelPeripheralConnection(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectWaiters.removeValue(forKey: peripheral.identifier)?.resume()
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        connectWaiters.removeValue(forKey: peripheral.identifier)?
            .resume(throwing: error ?? TransportError(message: "could not connect"))
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        connectWaiters.removeValue(forKey: peripheral.identifier)?
            .resume(throwing: error ?? TransportError(message: "disconnected"))
        transports.removeValue(forKey: peripheral.identifier)?.handleDisconnect()
    }
}

/// One Bluetooth connection to a MokaBot.
final class BleTransport: NSObject, Transport, CBPeripheralDelegate {
    let peripheral: CBPeripheral
    let name: String
    var label: String { "Bluetooth \(name)" }
    var onDisconnected: (() -> Void)?

    private var rx: CBCharacteristic?
    private var tx: CBCharacteristic?
    private var setupWaiter: CheckedContinuation<Void, Error>?
    private var responseWaiter: CheckedContinuation<Data, Error>?
    private var pendingPackets: [Data] = []
    private var buffer = Data()
    private var requestSerial = 0
    private var busy = false
    private var lockWaiters: [CheckedContinuation<Void, Never>] = []
    private var closed = false

    init(peripheral: CBPeripheral, name: String) {
        self.peripheral = peripheral
        self.name = name
        super.init()
        peripheral.delegate = self
    }

    // ---- Set-up: service, characteristics, notifications ----

    @MainActor
    func setUp() async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            setupWaiter = continuation
            peripheral.discoverServices([MokaBle.service])
        }
    }

    private func failSetUp(_ message: String) {
        setupWaiter?.resume(throwing: TransportError(message: message))
        setupWaiter = nil
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == MokaBle.service }) else {
            return failSetUp("not a MokaBot")
        }
        peripheral.discoverCharacteristics([MokaBle.rx, MokaBle.tx], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        rx = service.characteristics?.first(where: { $0.uuid == MokaBle.rx })
        tx = service.characteristics?.first(where: { $0.uuid == MokaBle.tx })
        guard let tx = tx, rx != nil else { return failSetUp("not a MokaBot") }
        // Responses arrive as notifications.
        peripheral.setNotifyValue(true, for: tx)
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error { return failSetUp(error.localizedDescription) }
        setupWaiter?.resume()
        setupWaiter = nil
    }

    // ---- Requests (one at a time) ----

    @MainActor
    private func lock() async {
        if !busy {
            busy = true
            return
        }
        await withCheckedContinuation { lockWaiters.append($0) }
    }

    private func unlock() {
        if lockWaiters.isEmpty { busy = false } else { lockWaiters.removeFirst().resume() }
    }

    @MainActor
    func request(method: String, target: String, body: Data?) async throws -> DeviceResponse {
        await lock()
        defer { unlock() }
        guard !closed, rx != nil else { throw TransportError(message: "not connected") }

        var message = Data("\(method) \(target)\n".utf8)
        if let body = body { message.append(body) }
        let chunk = max(peripheral.maximumWriteValueLength(for: .withResponse) - 1, 19)
        var packets: [Data] = []
        var offset = 0
        repeat {
            let length = min(chunk, message.count - offset)
            let last = offset + length >= message.count
            var packet = Data([last ? MokaBle.lastPacket : 0])
            packet.append(message.subdata(in: offset..<offset + length))
            packets.append(packet)
            offset += length
        } while offset < message.count

        requestSerial += 1
        let serial = requestSerial
        let raw: Data = try await withCheckedThrowingContinuation { continuation in
            responseWaiter = continuation
            buffer = Data()
            pendingPackets = packets
            writeNextPacket()
            // Large answers (a whole brew history) take a few seconds.
            DispatchQueue.main.asyncAfter(deadline: .now() + 30) { [weak self] in
                guard let self = self, self.requestSerial == serial else { return }
                self.failResponse(TransportError(message: "timed out"))
            }
        }
        return parse(raw)
    }

    private func writeNextPacket() {
        guard let rx = rx, !pendingPackets.isEmpty else { return }
        peripheral.writeValue(pendingPackets.removeFirst(), for: rx, type: .withResponse)
    }

    func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        if let error = error { return failResponse(error) }
        writeNextPacket()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == MokaBle.tx, let packet = characteristic.value, !packet.isEmpty else { return }
        buffer.append(packet.dropFirst())
        if packet[packet.startIndex] & MokaBle.lastPacket != 0 {
            let message = buffer
            buffer = Data()
            requestSerial += 1   // the timeout no longer applies
            responseWaiter?.resume(returning: message)
            responseWaiter = nil
        }
    }

    private func failResponse(_ error: Error) {
        pendingPackets = []
        responseWaiter?.resume(throwing: error)
        responseWaiter = nil
    }

    private func parse(_ raw: Data) -> DeviceResponse {
        let lineEnd = raw.firstIndex(of: UInt8(ascii: "\n")) ?? raw.endIndex
        let line = String(decoding: raw[raw.startIndex..<lineEnd], as: UTF8.self)
        let parts = line.split(separator: " ", maxSplits: 1)
        let status = parts.first.flatMap { Int($0) } ?? 500
        let type = parts.count > 1 ? String(parts[1]) : "text/plain"
        let body = lineEnd < raw.endIndex ? raw[raw.index(after: lineEnd)...] : Data()
        return DeviceResponse(status: status, contentType: type, body: Data(body))
    }

    // ---- Disconnection ----

    func handleDisconnect() {
        failSetUp("disconnected")
        failResponse(TransportError(message: "disconnected"))
        if !closed {
            closed = true
            onDisconnected?()
        }
    }

    func close() {
        closed = true
        BleCentral.shared.disconnect(peripheral)
    }
}

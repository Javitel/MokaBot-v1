import SwiftUI

/// Connection screen: Bluetooth or WiFi. With several MokaBots in reach the
/// user picks one (by device name and signal); the app remembers the last
/// Bluetooth device and connects straight to it next time.
///
/// No WiFi "Search" here (unlike Android): iOS only lets apps send broadcasts
/// with an Apple-granted multicast entitlement. The address (IP or
/// <name>.local) is typed instead.
struct ConnectView: View {
    @EnvironmentObject private var model: AppModel
    @AppStorage("address") private var address = "mokabot.local"
    @AppStorage("bluetoothDevice") private var lastDevice = ""
    @State private var busy = false
    @State private var status = ""
    @State private var choices: [FoundDevice] = []
    @State private var showChoices = false

    var body: some View {
        NavigationView {
            Form {
                Section(footer: Text("Connects to the last MokaBot used, or lists them when several are in reach.")) {
                    Button("Connect via Bluetooth") { connectBluetooth(choose: false) }
                    Button("Choose another device…") { connectBluetooth(choose: true) }
                }
                Section(footer: Text("The address shown on the MokaBot screen (IP button), or <name>.local.")) {
                    TextField("Address (IP or mokabot.local)", text: $address)
                        .keyboardType(.URL)
                        .textInputAutocapitalization(.never)
                        .disableAutocorrection(true)
                    Button("Connect via WiFi") { connectWifi() }
                }
                if busy || !status.isEmpty || !model.message.isEmpty {
                    Section {
                        HStack {
                            if busy { ProgressView() }
                            Text(status.isEmpty ? model.message : status)
                        }
                    }
                }
            }
            .disabled(busy)
            .navigationTitle("MokaBot")
            .confirmationDialog("Choose your MokaBot", isPresented: $showChoices, titleVisibility: .visible) {
                ForEach(choices) { device in
                    Button(label(for: device)) { openBluetooth(device) }
                }
            }
        }
        .navigationViewStyle(.stack)
    }

    private func label(for device: FoundDevice) -> String {
        let signal = device.rssi >= -60 ? "near" : (device.rssi >= -75 ? "medium" : "far")
        var label = "\(device.name) — \(signal) (\(device.rssi) dBm)"
        if device.id.uuidString == lastDevice { label += " · last used" }
        return label
    }

    private func fail(_ error: Error) {
        busy = false
        status = "Could not connect: \(error.localizedDescription)"
    }

    /// Without `choose`: connects to the last device used as soon as it is
    /// seen, or to the only one found; with several, or with `choose`, shows the list.
    private func connectBluetooth(choose: Bool) {
        busy = true
        status = "Searching for MokaBot over Bluetooth…"
        Task { @MainActor in
            do {
                let last = UUID(uuidString: lastDevice)
                let found = try await BleCentral.shared.scan(stopOn: choose ? nil : last)
                let lastFound = found.first { $0.id == last }
                if found.isEmpty {
                    busy = false
                    status = "No MokaBot found over Bluetooth. Is it switched on and nearby?"
                } else if !choose, let device = lastFound {
                    openBluetooth(device)
                } else if !choose && found.count == 1 {
                    openBluetooth(found[0])
                } else {
                    busy = false
                    status = ""
                    choices = found
                    showChoices = true
                }
            } catch {
                busy = false
                status = error.localizedDescription
            }
        }
    }

    private func openBluetooth(_ device: FoundDevice) {
        busy = true
        status = "Connecting to \(device.name)…"
        Task { @MainActor in
            do {
                let transport = try await BleCentral.shared.connect(device)
                lastDevice = device.id.uuidString
                busy = false
                status = ""
                model.open(transport)
            } catch {
                fail(error)
            }
        }
    }

    private func connectWifi() {
        let host = address.trimmingCharacters(in: .whitespaces)
            .replacingOccurrences(of: "http://", with: "")
            .trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard !host.isEmpty else { return }
        address = host
        busy = true
        status = "Connecting…"
        Task { @MainActor in
            do {
                let transport = try await WifiTransport.connect(host: host)
                busy = false
                status = ""
                model.open(transport)
            } catch {
                fail(error)
            }
        }
    }
}

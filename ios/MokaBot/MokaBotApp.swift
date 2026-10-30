import SwiftUI

@main
struct MokaBotApp: App {
    @StateObject private var model = AppModel()

    var body: some Scene {
        WindowGroup {
            ContentView()
                .environmentObject(model)
        }
    }
}

/// The connection in use: the connection screen until there is one, then the pages.
final class AppModel: ObservableObject {
    @Published private(set) var transport: Transport?
    /// Shown on the connection screen (e.g. after a Bluetooth disconnection).
    @Published var message = ""

    func open(_ transport: Transport) {
        self.transport?.close()
        transport.onDisconnected = { [weak self] in
            self?.transport = nil
            self?.message = "MokaBot disconnected"
        }
        message = ""
        self.transport = transport
    }

    func disconnect() {
        transport?.onDisconnected = nil
        transport?.close()
        transport = nil
    }
}

struct ContentView: View {
    @EnvironmentObject private var model: AppModel

    var body: some View {
        if let transport = model.transport {
            BrowserScreen(transport: transport)
        } else {
            ConnectView()
        }
    }
}

/// The pages, with Brew / Settings / Disconnect in the navigation bar.
struct BrowserScreen: View {
    @EnvironmentObject private var model: AppModel
    let transport: Transport
    @State private var request = PageRequest(page: .brew, serial: 0)

    var body: some View {
        NavigationView {
            BrowserView(transport: transport, request: request)
                .ignoresSafeArea(edges: .bottom)
                .navigationTitle(transport.label)
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItemGroup(placement: .navigationBarTrailing) {
                        Button("Brew") { request = PageRequest(page: .brew, serial: request.serial + 1) }
                        Button("Settings") { request = PageRequest(page: .settings, serial: request.serial + 1) }
                    }
                    ToolbarItem(placement: .navigationBarLeading) {
                        Button("Disconnect") { model.disconnect() }
                    }
                }
        }
        .navigationViewStyle(.stack)   // full width on iPad too
    }
}

import Foundation

/// Answer of the MokaBot to one request (same as its web server's).
struct DeviceResponse {
    let status: Int
    let contentType: String
    let body: Data
}

/// A connection to the MokaBot. The pages' data requests (/brewHistory,
/// /settingsInfo, /set...) go through it, over WiFi (HTTP) or Bluetooth LE.
protocol Transport: AnyObject {
    /// Short description for the title bar, e.g. "WiFi 192.168.1.20".
    var label: String { get }
    /// Called (on the main queue) when the connection drops (Bluetooth only).
    var onDisconnected: (() -> Void)? { get set }
    /// `target` is the path with its query, e.g. "/brewHistory?session=1&from=0".
    func request(method: String, target: String, body: Data?) async throws -> DeviceResponse
    func close()
}

struct TransportError: LocalizedError {
    let message: String
    var errorDescription: String? { message }
}

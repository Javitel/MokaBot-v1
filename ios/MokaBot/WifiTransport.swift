import Foundation

/// Plain HTTP to the MokaBot's web server on the local network.
final class WifiTransport: Transport {
    let host: String
    var label: String { "WiFi \(host)" }
    var onDisconnected: (() -> Void)?

    private let session: URLSession = {
        let configuration = URLSessionConfiguration.ephemeral
        configuration.timeoutIntervalForRequest = 15
        return URLSession(configuration: configuration)
    }()

    init(host: String) {
        self.host = host
    }

    /// Checks that a MokaBot answers at `host`.
    static func connect(host: String) async throws -> WifiTransport {
        let transport = WifiTransport(host: host)
        let response = try await transport.request(method: "GET", target: "/brewSlots", body: nil)
        guard response.status == 200 else { throw TransportError(message: "HTTP \(response.status)") }
        return transport
    }

    func request(method: String, target: String, body: Data?) async throws -> DeviceResponse {
        guard let url = URL(string: "http://\(host)\(target)") else {
            throw TransportError(message: "invalid address")
        }
        var request = URLRequest(url: url)
        request.httpMethod = method
        if let body = body {
            request.httpBody = body
            request.setValue("text/plain", forHTTPHeaderField: "Content-Type")
        }
        let (data, response) = try await session.data(for: request)
        let http = response as? HTTPURLResponse
        let type = (http?.value(forHTTPHeaderField: "Content-Type") ?? "text/plain")
            .components(separatedBy: ";")[0]
        return DeviceResponse(status: http?.statusCode ?? 500, contentType: type, body: data)
    }

    func close() {
        session.invalidateAndCancel()
    }
}

import SwiftUI
import UIKit
import WebKit

/// The firmware's pages bundled in the app, at moka://localhost/app/...
enum Page {
    case brew, settings

    var url: URL {
        switch self {
        case .brew: return URL(string: "\(PageSchemeHandler.scheme)://localhost/app/brew")!
        case .settings: return URL(string: "\(PageSchemeHandler.scheme)://localhost/app/settings")!
        }
    }
}

/// A page to load; `serial` changes on every menu tap, so tapping the page
/// already shown loads it again.
struct PageRequest: Equatable {
    var page: Page
    var serial: Int
}

/// Shows the brew and settings pages in a WKWebView. Their data requests are
/// routed through the transport by web/app/bridge.js, so they work over WiFi
/// and Bluetooth alike (same design as the Android app's BrowserActivity).
struct BrowserView: UIViewRepresentable {
    let transport: Transport
    let request: PageRequest

    func makeCoordinator() -> Coordinator {
        Coordinator(transport: transport)
    }

    func makeUIView(context: Context) -> WKWebView {
        let configuration = WKWebViewConfiguration()
        configuration.setURLSchemeHandler(PageSchemeHandler(), forURLScheme: PageSchemeHandler.scheme)
        configuration.userContentController.add(WeakMessageHandler(context.coordinator), name: "mokaBridge")
        let webView = WKWebView(frame: .zero, configuration: configuration)
        webView.uiDelegate = context.coordinator
        context.coordinator.webView = webView
        return webView
    }

    func updateUIView(_ webView: WKWebView, context: Context) {
        if context.coordinator.loadedSerial != request.serial {
            context.coordinator.loadedSerial = request.serial
            webView.load(URLRequest(url: request.page.url))
        }
    }

    static func dismantleUIView(_ webView: WKWebView, coordinator: Coordinator) {
        webView.configuration.userContentController.removeScriptMessageHandler(forName: "mokaBridge")
    }

    final class Coordinator: NSObject, WKScriptMessageHandler, WKUIDelegate {
        let transport: Transport
        weak var webView: WKWebView?
        var loadedSerial = -1

        init(transport: Transport) {
            self.transport = transport
        }

        // ---- Bridge (bridge.js) ----

        func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
            guard let body = message.body as? [String: Any], let type = body["type"] as? String else { return }
            if type == "request" {
                let id = (body["id"] as? NSNumber)?.intValue ?? 0
                let method = body["method"] as? String ?? "GET"
                let target = body["target"] as? String ?? "/"
                let requestBody = (body["body"] as? String).map { Data($0.utf8) }
                Task { @MainActor in
                    do {
                        let response = try await self.transport.request(method: method, target: target, body: requestBody)
                        self.deliver(id: id, status: response.status, type: response.contentType, body: response.body)
                    } catch {
                        // Status 0: the page's fetch() rejects, as on a network error.
                        self.deliver(id: id, status: 0, type: "text/plain", body: Data(error.localizedDescription.utf8))
                    }
                }
            } else if type == "saveFile", let name = body["name"] as? String, let text = body["text"] as? String {
                saveFile(name: name, text: text)
            }
        }

        private func deliver(id: Int, status: Int, type: String, body: Data) {
            let script = "window.__mokaBridgeDone(\(id),\(status),\(jsString(type)),\"\(body.base64EncodedString())\")"
            webView?.evaluateJavaScript(script, completionHandler: nil)
        }

        private func jsString(_ value: String) -> String {
            guard let data = try? JSONSerialization.data(withJSONObject: [value]),
                  let array = String(data: data, encoding: .utf8) else { return "\"\"" }
            return String(array.dropFirst().dropLast())   // ["..."] -> "..."
        }

        /// Brew download from the settings page: saved with the share sheet
        /// (Save to Files, AirDrop...).
        private func saveFile(name: String, text: String) {
            let url = FileManager.default.temporaryDirectory.appendingPathComponent(name)
            do {
                try Data(text.utf8).write(to: url)
            } catch {
                return
            }
            let sheet = UIActivityViewController(activityItems: [url], applicationActivities: nil)
            if let webView = webView {
                // iPad: shown as a popover from the page.
                sheet.popoverPresentationController?.sourceView = webView
                sheet.popoverPresentationController?.sourceRect = CGRect(x: webView.bounds.midX, y: webView.bounds.midY, width: 0, height: 0)
            }
            topViewController()?.present(sheet, animated: true)
        }

        // ---- JavaScript dialogs (the pages use alert() and confirm()) ----

        func webView(_ webView: WKWebView, runJavaScriptAlertPanelWithMessage message: String,
                     initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping () -> Void) {
            let alert = UIAlertController(title: nil, message: message, preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "OK", style: .default) { _ in completionHandler() })
            if let top = topViewController() { top.present(alert, animated: true) } else { completionHandler() }
        }

        func webView(_ webView: WKWebView, runJavaScriptConfirmPanelWithMessage message: String,
                     initiatedByFrame frame: WKFrameInfo, completionHandler: @escaping (Bool) -> Void) {
            let alert = UIAlertController(title: nil, message: message, preferredStyle: .alert)
            alert.addAction(UIAlertAction(title: "Cancel", style: .cancel) { _ in completionHandler(false) })
            alert.addAction(UIAlertAction(title: "OK", style: .default) { _ in completionHandler(true) })
            if let top = topViewController() { top.present(alert, animated: true) } else { completionHandler(false) }
        }

        private func topViewController() -> UIViewController? {
            let window = UIApplication.shared.connectedScenes
                .compactMap { $0 as? UIWindowScene }
                .flatMap { $0.windows }
                .first { $0.isKeyWindow }
            var top = window?.rootViewController
            while let presented = top?.presentedViewController { top = presented }
            return top
        }
    }
}

/// WKUserContentController keeps its handlers alive: this breaks the cycle
/// with the coordinator.
final class WeakMessageHandler: NSObject, WKScriptMessageHandler {
    weak var target: WKScriptMessageHandler?

    init(_ target: WKScriptMessageHandler) {
        self.target = target
    }

    func userContentController(_ userContentController: WKUserContentController, didReceive message: WKScriptMessage) {
        target?.userContentController(userContentController, didReceive: message)
    }
}

/// Serves moka://localhost/...: the pages (with the bridge loaded first and
/// their CDN libraries replaced by the bundled copies), the bridge and the
/// libraries.
final class PageSchemeHandler: NSObject, WKURLSchemeHandler {
    static let scheme = "moka"

    func webView(_ webView: WKWebView, start task: WKURLSchemeTask) {
        guard let url = task.request.url else { return }
        var status = 200
        var type = "text/plain"
        var data = Data()
        switch url.path {
        case "/app", "/app/", "/app/brew", "/app/brew.html":
            (type, data) = ("text/html", page(named: "brew"))
        case "/app/settings", "/app/settings.html":
            (type, data) = ("text/html", page(named: "settings"))
        case let path where path.hasPrefix("/app/") || path.hasPrefix("/vendor/"):
            if let file = webFile(String(path.dropFirst())) {
                (type, data) = (path.hasSuffix(".js") ? "text/javascript" : "application/octet-stream", file)
            } else {
                status = 404
            }
        default:
            status = 404   // e.g. the device's /apple-touch-icon.png and /manifest.json
        }
        let response = HTTPURLResponse(url: url, statusCode: status, httpVersion: "HTTP/1.1",
                                       headerFields: ["Content-Type": type])!
        task.didReceive(response)
        task.didReceive(data)
        task.didFinish()
    }

    func webView(_ webView: WKWebView, stop task: WKURLSchemeTask) {}

    private func webFile(_ relativePath: String) -> Data? {
        guard let root = Bundle.main.resourceURL else { return nil }
        return try? Data(contentsOf: root.appendingPathComponent("web").appendingPathComponent(relativePath))
    }

    private func page(named name: String) -> Data {
        guard let url = Bundle.main.url(forResource: name, withExtension: "html"),
              var html = try? String(contentsOf: url, encoding: .utf8) else { return Data() }
        if let head = html.range(of: "<head>") {
            html.replaceSubrange(head, with: "<head><script src=\"/app/bridge.js\"></script>")
        }
        html = html.replacingOccurrences(of: "https://cdn\\.jsdelivr\\.net/npm/chart\\.js@[^\"]*",
                                         with: "/vendor/chart.umd.min.js", options: .regularExpression)
        html = html.replacingOccurrences(of: "https://cdn\\.jsdelivr\\.net/npm/@jaames/iro@[^\"]*",
                                         with: "/vendor/iro.min.js", options: .regularExpression)
        return Data(html.utf8)
    }
}

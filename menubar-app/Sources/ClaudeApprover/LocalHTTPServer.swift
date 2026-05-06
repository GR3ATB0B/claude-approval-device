// LocalHTTPServer — minimal HTTP/1.1 server on 127.0.0.1 used by the Python
// MCP server to forward LED/jingle/tone commands from Claude Code into the
// running BLE bond. Loopback only, no auth needed (local trust boundary).

import Foundation
import Network

private let DEFAULT_PORT: UInt16 = 47823

final class LocalHTTPServer {
    typealias Handler = (_ method: String, _ path: String, _ body: Data) -> (status: Int, body: Data)

    private var listener: NWListener?
    private let queue = DispatchQueue(label: "approver.http")
    private let handler: Handler

    init(handler: @escaping Handler) {
        self.handler = handler
    }

    func start(port: UInt16 = DEFAULT_PORT) throws {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        params.requiredInterfaceType = .loopback
        let l = try NWListener(using: params, on: .init(rawValue: port)!)
        listener = l
        l.newConnectionHandler = { [weak self] conn in
            self?.handle(conn)
        }
        l.start(queue: queue)
        NSLog("[HTTP] listening on 127.0.0.1:\(port)")
    }

    func stop() {
        listener?.cancel()
        listener = nil
    }

    private func handle(_ conn: NWConnection) {
        conn.start(queue: queue)
        receive(conn, accumulated: Data())
    }

    private func receive(_ conn: NWConnection, accumulated: Data) {
        conn.receive(minimumIncompleteLength: 1, maximumLength: 64 * 1024) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            var buf = accumulated
            if let d = data, !d.isEmpty { buf.append(d) }
            if let parsed = self.parseRequest(buf) {
                let (method, path, body) = parsed
                let (status, respBody) = self.handler(method, path, body)
                self.respond(conn, status: status, body: respBody)
                return
            }
            if isComplete || error != nil {
                conn.cancel()
                return
            }
            self.receive(conn, accumulated: buf)
        }
    }

    /// Returns (method, path, body) once a complete HTTP/1.1 request has been buffered.
    private func parseRequest(_ buf: Data) -> (String, String, Data)? {
        guard let separator = buf.range(of: Data("\r\n\r\n".utf8)) else {
            return nil
        }
        let head = buf.prefix(upTo: separator.lowerBound)
        let bodyStart = separator.upperBound
        guard let headStr = String(data: head, encoding: .utf8) else { return nil }
        let lines = headStr.components(separatedBy: "\r\n")
        guard let requestLine = lines.first else { return nil }
        let parts = requestLine.split(separator: " ", maxSplits: 2, omittingEmptySubsequences: false)
        guard parts.count >= 2 else { return nil }
        let method = String(parts[0])
        let path = String(parts[1])

        var contentLength = 0
        for line in lines.dropFirst() {
            if line.lowercased().hasPrefix("content-length:") {
                let value = line.dropFirst("content-length:".count)
                    .trimmingCharacters(in: .whitespaces)
                contentLength = Int(value) ?? 0
                break
            }
        }
        let bodyAvailable = buf.count - bodyStart
        if bodyAvailable < contentLength { return nil }
        let body = buf.subdata(in: bodyStart..<bodyStart + contentLength)
        return (method, path, body)
    }

    private func respond(_ conn: NWConnection, status: Int, body: Data) {
        let reason: String
        switch status {
        case 200: reason = "OK"
        case 400: reason = "Bad Request"
        case 404: reason = "Not Found"
        case 503: reason = "Service Unavailable"
        default:  reason = "Error"
        }
        var head = "HTTP/1.1 \(status) \(reason)\r\n"
        head += "Content-Type: application/json\r\n"
        head += "Content-Length: \(body.count)\r\n"
        head += "Connection: close\r\n\r\n"
        var out = Data(head.utf8)
        out.append(body)
        conn.send(content: out, completion: .contentProcessed { _ in
            conn.cancel()
        })
    }
}

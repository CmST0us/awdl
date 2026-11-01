import Foundation

/// One request and its correlated response on a TCP byte stream.
struct FirstPacketExchange {
    let runID: String
    let messageID = "1"
    private(set) var received = Data()
    var request: Data { Data("HELLO \(runID) \(messageID)\n".utf8) }
    var response: Data { Data("ACK \(runID) \(messageID)\n".utf8) }

    enum Failure: Error { case unexpectedResponse, truncatedResponse }

    mutating func consume(_ data: Data, complete: Bool) throws -> Bool {
        received.append(data)
        guard received.count <= 256, response.starts(with: received) else {
            throw Failure.unexpectedResponse
        }
        if received == response { return true }
        if complete { throw Failure.truncatedResponse }
        return false
    }
}

import Foundation

enum ClientConfiguration {
    static let serviceName = Bundle.main.object(forInfoDictionaryKey: "AwareFileService") as! String
    private static let policy: [String: Any] = {
        let url = Bundle.main.url(forResource: "Client", withExtension: "plist")!
        return try! PropertyListSerialization.propertyList(from: Data(contentsOf: url), format: nil) as! [String: Any]
    }()
    private static func seconds(_ key: String) -> TimeInterval {
        let seconds = (policy[key] as! NSNumber).doubleValue
        precondition(seconds > 0, "客户端期限必须为正整数秒")
        return seconds
    }
    static let connectionTimeout = seconds("ConnectionTimeoutSeconds")
    static let ioIdleTimeout = seconds("IOIdleTimeoutSeconds")
}

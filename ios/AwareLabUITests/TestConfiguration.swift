import Foundation

final class TestConfiguration {
    private static let values: NSDictionary = {
        let bundle = Bundle(for: TestConfiguration.self)
        guard let url = bundle.url(forResource: "AwareLab-Info", withExtension: "plist"),
              let values = NSDictionary(contentsOf: url) else {
            preconditionFailure("Missing file service contract in test resources")
        }
        return values
    }()
    static let service = values["AwareFileService"] as! String
    static let targetName = ProcessInfo.processInfo.environment["AWARE_TARGET_NAME"] ?? values["AwarePairingName"] as! String
    static let vendorName = ProcessInfo.processInfo.environment["AWARE_VENDOR_NAME"] ?? values["AwareVendorName"] as! String
    static let modelName = ProcessInfo.processInfo.environment["AWARE_MODEL_NAME"] ?? values["AwareModelName"] as! String
    static let displayItemName = ProcessInfo.processInfo.environment["AWARE_DISPLAY_ITEM_NAME"] ?? modelName
    static let clientBundle = values["AwareClientBundle"] as! String
    static let clientDisplayName = values["AwareClientDisplayName"] as! String
}

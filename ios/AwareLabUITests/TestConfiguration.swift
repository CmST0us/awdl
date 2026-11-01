import Foundation

final class TestConfiguration {
    static let service: String = {
        let bundle = Bundle(for: TestConfiguration.self)
        guard let url = bundle.url(forResource: "AwareLab-Info", withExtension: "plist"),
              let values = NSDictionary(contentsOf: url),
              let service = values["AwareFileService"] as? String else {
            preconditionFailure("Missing file service contract in test resources")
        }
        return service
    }()
}

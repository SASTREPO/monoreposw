//
// Templates UnitTests
// Copyright © 2019 SwiftGen
// MIT Licence
//

import StencilSwiftKit
import XCTest

class FontsTests: XCTestCase {
  enum Contexts {
    static let all = ["empty", "defaults"]
  }

  // generate variations to test customname generation
  let variations: VariationGenerator = { name, context in
    guard name == "defaults" else { return [(context: context, suffix: "")] }

    return [
      (
        context: context,
        suffix: ""
      ),
      (
        context: try StencilContext.enrich(context: context, parameters: ["enumName=CustomFamily"]),
        suffix: "-customName"
      ),
      (
        context: try StencilContext.enrich(context: context, parameters: ["preservePath"]),
        suffix: "-preservePath"
      ),
      (
        context: try StencilContext.enrich(context: context, parameters: ["publicAccess"]),
        suffix: "-publicAccess"
      )
    ]
  }

  func testSwift4() {
    test(
      template: "swift4",
      contextNames: Contexts.all,
      directory: .fonts,
      contextVariations: variations
    )
  }

  func testSwift5() {
    test(
      template: "swift5",
      contextNames: Contexts.all,
      directory: .fonts,
      contextVariations: variations
    )
  }
}

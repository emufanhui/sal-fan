using Cavern.Format.Decoders;
using Cavern.Format.Renderers;
using Cavern.Format.Utilities;

namespace Test.Cavern.Format.Renderers;

/// <summary>
/// Tests E-AC-3 renderer lifetime handling.
/// </summary>
[TestClass]
public class EnhancedAC3Renderer_Tests {
    /// <summary>
    /// Channel-only E-AC-3 streams don't create a JOC applier and must still dispose cleanly.
    /// </summary>
    [TestMethod]
    public void Dispose_without_objects_does_not_throw() {
        BlockBuffer<byte> input = new(() => null);
        EnhancedAC3Decoder decoder = new(input);
        EnhancedAC3Renderer renderer = new(decoder);

        renderer.Dispose();
    }
}

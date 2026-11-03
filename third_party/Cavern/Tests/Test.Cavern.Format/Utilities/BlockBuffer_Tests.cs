using Cavern.Format.Utilities;

namespace Test.Cavern.Format.Utilities;

/// <summary>
/// Tests buffered packet boundaries.
/// </summary>
[TestClass]
public class BlockBuffer_Tests {
    /// <summary>
    /// Completing an exact read must not fetch the next packet until another read starts.
    /// </summary>
    [TestMethod]
    public void Exact_read_does_not_prefetch_next_packet() {
        int fetches = 0;
        BlockBuffer<byte> buffer = new(() => ++fetches switch {
            1 => [1, 2],
            2 => [3, 4],
            _ => null,
        });

        CollectionAssert.AreEqual(new byte[] { 1, 2 }, buffer.Read(2));
        Assert.AreEqual(1, fetches);
        Assert.AreEqual(3, buffer.ReadOne());
        Assert.AreEqual(2, fetches);
    }
}

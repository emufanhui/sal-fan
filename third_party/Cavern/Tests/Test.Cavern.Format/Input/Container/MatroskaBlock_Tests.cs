using System.Reflection;
using Cavern.Format.Container.Matroska;

namespace Test.Cavern.Format.Input.Container;

/// <summary>
/// Tests raw frame extraction from Matroska blocks.
/// </summary>
[TestClass]
public class MatroskaBlock_Tests {
    /// <summary>
    /// Lacing descriptors are container metadata and must not leak into codec data.
    /// </summary>
    [DataTestMethod]
    [DataRow((byte)0x02, (byte)0x03)] // Xiph lacing
    [DataRow((byte)0x06, (byte)0x83)] // EBML lacing
    public void Lacing_descriptors_are_excluded_from_frame_data(byte lacing, byte firstFrameSize) {
        byte[] expected = [0x11, 0x22, 0x33, 0x44, 0x55];
        byte[] blockPayload = [
            0x81, 0x00, 0x00, (byte)(0x80 | lacing),
            0x01, firstFrameSize,
            .. expected
        ];
        byte[] element = [0xA3, (byte)(0x80 | blockPayload.Length), .. blockPayload];
        using MemoryStream stream = new(element);
        MatroskaTree tree = new(stream);
        Type blockType = typeof(MatroskaTree).Assembly.GetType(
            "Cavern.Format.Container.Matroska.Block", throwOnError: true)!;
        object block = Activator.CreateInstance(
            blockType,
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
            binder: null,
            args: [stream, tree],
            culture: null)!;

        byte[] actual = (byte[])blockType.GetMethod("GetData")!.Invoke(block, null)!;
        byte[][] frames = (byte[][])blockType.GetMethod("GetFrames")!.Invoke(block, null)!;

        CollectionAssert.AreEqual(expected, actual);
        Assert.AreEqual(2, frames.Length);
        CollectionAssert.AreEqual(expected[..3], frames[0]);
        CollectionAssert.AreEqual(expected[3..], frames[1]);
    }
}

using System.Reflection;

using Cavern.Format.Decoders;

namespace Test.Cavern.Format.Decoders.EnhancedAC3;

/// <summary>
/// Tests joint object coding timeslot processing.
/// </summary>
[TestClass]
public class JointObjectCodingApplier_Tests {
    /// <summary>
    /// The timeslot cursor follows QMF blocks in a frame, not the input channel count.
    /// </summary>
    [TestMethod, Timeout(5000)]
    public void Timeslot_wraps_at_qmf_blocks_per_frame() {
        Assembly assembly = typeof(EnhancedAC3Decoder).Assembly;
        Type jocType = assembly.GetType(
            "Cavern.Format.Decoders.EnhancedAC3.JointObjectCoding", throwOnError: true)!;
        object joc = Activator.CreateInstance(jocType)!;
        jocType.GetProperty("ChannelCount")!.SetValue(joc, 5);
        jocType.GetProperty("ObjectCount")!.SetValue(joc, 1);
        jocType.GetMethod("UpdateCache")!.Invoke(joc, null);

        Type applierType = assembly.GetType(
            "Cavern.Format.Decoders.EnhancedAC3.JointObjectCodingApplier",
            throwOnError: true)!;
        const int frameSize = 256;
        object applier = Activator.CreateInstance(
            applierType,
            BindingFlags.Instance | BindingFlags.Public | BindingFlags.NonPublic,
            binder: null,
            args: [joc, frameSize],
            culture: null)!;
        MethodInfo apply = applierType.GetMethod("Apply")!;
        FieldInfo timeslot = applierType.GetField(
            "timeslot", BindingFlags.Instance | BindingFlags.NonPublic)!;
        float[][] input = Enumerable.Range(0, 5).Select(_ => new float[64]).ToArray();

        try {
            for (int i = 0; i < frameSize / 64; ++i) {
                apply.Invoke(applier, [input, joc]);
            }

            Assert.AreEqual(0, (int)timeslot.GetValue(applier)!);
        } finally {
            ((IDisposable)applier).Dispose();
            ((IDisposable)joc).Dispose();
        }
    }
}

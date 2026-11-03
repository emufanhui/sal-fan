using System;
using System.IO;

using Cavern;
using Cavern.Channels;
using Cavern.Format;
using Cavern.Format.Common;
using Cavern.Format.Renderers;
using Cavern.Format.Utilities;
using Cavern.Utilities;

// Render a truehdd-generated Dolby Atmos Master Format (.atmos) file to
// a 12-channel 7.1.4 PCM WAV. This is the offline reference path before a
// live TrueHD decoder is connected to the existing PCM714 bridge.
const int outputChannels = 12;
const int updateRate = 1536;

if (args.Length < 2 || (args[0] != "inspect" && args[0] != "render")) {
    PrintUsage();
    return 64;
}

string command = args[0];
string input = Path.GetFullPath(args[1]);
if (!File.Exists(input)) {
    Console.Error.WriteLine($"Input file not found: {input}");
    return 66;
}

using AudioReader reader = AudioReader.Open(input);
reader.ReadHeader();
using Renderer renderer = reader.GetRenderer();
bool directTrueHd = reader is AudioTrackReader trackReader && trackReader.track.Format == Codec.TrueHD;

Console.WriteLine($"Input:       {input}");
Console.WriteLine($"Format:      {reader.GetType().Name}");
Console.WriteLine($"Sample rate: {reader.SampleRate} Hz");
Console.WriteLine($"Core:        {reader.ChannelCount} channels");
Console.WriteLine($"Objects:     {renderer.Objects.Count}");
Console.WriteLine($"HasObjects:  {renderer.HasObjects}");
if (directTrueHd) {
    Console.Error.WriteLine("Direct TrueHD rendering is not supported by Cavern. Decode presentation 3 with truehdd first.");
    return 3;
}

if (command == "inspect") {
    if (reader is IMetadataSupplier supplier) {
        ReadableMetadata metadata = supplier.GetMetadata();
        if (metadata != null) {
            foreach (ReadableMetadataHeader header in metadata.Headers) {
                Console.WriteLine($"[{header.Name}]");
                foreach (ReadableMetadataField field in header.Fields) {
                    Console.WriteLine($"  {field}");
                }
            }
        }
    }
    return renderer.HasObjects ? 0 : 2;
}

if (args.Length < 3) {
    PrintUsage();
    return 64;
}

string output = Path.GetFullPath(args[2]);
double requestedSeconds = args.Length >= 4 ?
    double.Parse(args[3], System.Globalization.CultureInfo.InvariantCulture) :
    0;
if (requestedSeconds <= 0) {
    requestedSeconds = reader.Length / (double)reader.SampleRate;
}
if (requestedSeconds <= 0 || reader.Length <= 0) {
    Console.Error.WriteLine("Could not determine a finite input duration.");
    return 64;
}
double gain = args.Length >= 5 ?
    double.Parse(args[4], System.Globalization.CultureInfo.InvariantCulture) :
    (renderer.HasObjects ? .707 : 1.0);
if (!double.IsFinite(gain) || gain < 0 || gain > 2) {
    Console.Error.WriteLine("Gain must be between 0 and 2.");
    return 64;
}

long outputFrames = Math.Min(reader.Length, (long)Math.Ceiling(requestedSeconds * reader.SampleRate));
Listener.ReplaceChannels(ChannelPrototype.ToLayout(ChannelPrototype.ref714));
Listener listener = new(false) {
    SampleRate = reader.SampleRate,
    UpdateRate = updateRate,
    AudioQuality = QualityModes.Perfect,
    Volume = (float)gain,
};
listener.AttachSources(renderer.Objects);

Directory.CreateDirectory(Path.GetDirectoryName(output) ?? ".");
using RIFFWaveWriter writer = new(
    output, ChannelPrototype.ref714, outputFrames, reader.SampleRate, BitDepth.Int16);
writer.WriteHeader();

double[] sumSquares = new double[outputChannels];
float[] peaks = new float[outputChannels];
long renderedFrames = 0;
while (renderedFrames < outputFrames) {
    float[] block = listener.Render();
    int frames = (int)Math.Min(updateRate, outputFrames - renderedFrames);
    int samples = frames * outputChannels;
    writer.WriteBlock(block, 0, samples);

    for (int frame = 0; frame < frames; ++frame) {
        int offset = frame * outputChannels;
        for (int channel = 0; channel < outputChannels; ++channel) {
            float sample = block[offset + channel];
            sumSquares[channel] += sample * sample;
            peaks[channel] = Math.Max(peaks[channel], Math.Abs(sample));
        }
    }
    renderedFrames += frames;
}

string[] names = { "FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR", "TFL", "TFR", "TBL", "TBR" };
Console.WriteLine($"Output:      {output}");
Console.WriteLine($"Rendered:    {renderedFrames / (double)reader.SampleRate:F3} s, PCM16, 7.1.4");
Console.WriteLine("Channel RMS / peak:");
for (int channel = 0; channel < outputChannels; ++channel) {
    double rms = Math.Sqrt(sumSquares[channel] / renderedFrames);
    Console.WriteLine($"  {names[channel],3}: {ToDb(rms),7:F2} dBFS / {ToDb(peaks[channel]),7:F2} dBFS");
}
return 0;

static double ToDb(double value) => value <= 0 ? double.NegativeInfinity : 20 * Math.Log10(value);

static void PrintUsage() {
    Console.Error.WriteLine("Cavern714 inspect <input.atmos>");
    Console.Error.WriteLine("Cavern714 render <input.atmos> <output.wav> [seconds] [gain]");
}

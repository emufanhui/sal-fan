using System.IO;

namespace Ec3Recorder;

/// Resolves the SpatialAudioLab.CLI that carries the ec3-record / ec3-status
/// commands. The recorder is a standalone tool: it walks up from its own
/// location and takes the first CLI it finds, so it works from the repo build
/// output and from a copied folder alike.
internal static class CliLocator
{
    public static string? Find()
    {
        var candidates = new List<string>();

        string baseDirectory = AppContext.BaseDirectory;
        candidates.Add(Path.Combine(baseDirectory, "SpatialAudioLab.CLI.exe"));
        candidates.Add(Path.Combine(baseDirectory, "Engine", "SpatialAudioLab.CLI.exe"));

        DirectoryInfo? directory = new(baseDirectory);
        for (int depth = 0; depth < 6 && directory is not null; depth++)
        {
            candidates.Add(Path.Combine(directory.FullName, "build", "SpatialAudioLab.CLI.exe"));
            candidates.Add(Path.Combine(directory.FullName, "Engine", "SpatialAudioLab.CLI.exe"));
            directory = directory.Parent;
        }

        return candidates.FirstOrDefault(File.Exists);
    }
}

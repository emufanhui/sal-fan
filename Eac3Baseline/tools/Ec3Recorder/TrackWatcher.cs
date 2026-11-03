using System.IO;
using Windows.Media.Control;

namespace Ec3Recorder;

/// A track as reported by the system media session.
internal sealed record TrackInfo(string Title, string Artist)
{
    /// Apple Music packs "Artist — Album" into the artist field and leaves
    /// AlbumTitle empty, so the leading segment is the only usable artist.
    public string PrimaryArtist
    {
        get
        {
            int separator = Artist.IndexOf(" — ", StringComparison.Ordinal);
            string value = separator > 0 ? Artist[..separator] : Artist;
            return value.Trim();
        }
    }

    public string Display => string.IsNullOrWhiteSpace(PrimaryArtist)
        ? Title
        : $"{PrimaryArtist} — {Title}";

    /// Identity for split purposes. Artist is included because consecutive
    /// tracks can share a title across different albums.
    public string Key => $"{PrimaryArtist}\u001f{Title}";
}

/// Polls the Apple Music media session and reports track changes. Polling is
/// used rather than the session's own events because the events would need a
/// WinRT message pump we do not otherwise run.
internal sealed class TrackWatcher : IDisposable
{
    private const string AppIdFragment = "AppleMusic";

    // A track change is only reported after the same identity is seen twice,
    // which filters out the transient empty/placeholder values Apple Music
    // publishes while it is switching sources.
    private const int ConfirmPolls = 2;

    private CancellationTokenSource? _cts;
    private Task? _loop;

    /// Raised with the new track, or null when Apple Music stops being the
    /// active session. Always raised on the thread pool.
    public event Action<TrackInfo?>? TrackChanged;

    public void Start(int pollMilliseconds = 250)
    {
        if (_loop is not null) return;
        _cts = new CancellationTokenSource();
        CancellationToken token = _cts.Token;
        _loop = Task.Run(() => PollAsync(pollMilliseconds, token), token);
    }

    public void Stop()
    {
        _cts?.Cancel();
        _cts = null;
        _loop = null;
    }

    public void Dispose() => Stop();

    private async Task PollAsync(int pollMilliseconds, CancellationToken token)
    {
        GlobalSystemMediaTransportControlsSessionManager manager;
        try
        {
            manager = await GlobalSystemMediaTransportControlsSessionManager.RequestAsync();
        }
        catch
        {
            return;
        }

        string? reported = null;
        string? pendingKey = null;
        int pendingCount = 0;

        while (!token.IsCancellationRequested)
        {
            TrackInfo? current = await TryReadAsync(manager);
            string? key = current?.Key;

            if (key != reported)
            {
                if (key is not null && key == pendingKey)
                {
                    pendingCount++;
                }
                else
                {
                    pendingKey = key;
                    pendingCount = 1;
                }

                if (pendingCount >= ConfirmPolls)
                {
                    reported = key;
                    pendingKey = null;
                    pendingCount = 0;
                    TrackChanged?.Invoke(current);
                }
            }
            else
            {
                pendingKey = null;
                pendingCount = 0;
            }

            try
            {
                await Task.Delay(pollMilliseconds, token);
            }
            catch (OperationCanceledException)
            {
                return;
            }
        }
    }

    private static async Task<TrackInfo?> TryReadAsync(
        GlobalSystemMediaTransportControlsSessionManager manager)
    {
        try
        {
            foreach (GlobalSystemMediaTransportControlsSession session in manager.GetSessions())
            {
                if (!session.SourceAppUserModelId.Contains(AppIdFragment,
                        StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }

                if (session.GetPlaybackInfo().PlaybackStatus
                    != GlobalSystemMediaTransportControlsSessionPlaybackStatus.Playing)
                {
                    continue;
                }

                GlobalSystemMediaTransportControlsSessionMediaProperties props =
                    await session.TryGetMediaPropertiesAsync();
                string title = props.Title?.Trim() ?? string.Empty;
                if (title.Length == 0) continue;

                return new TrackInfo(title, props.Artist?.Trim() ?? string.Empty);
            }
        }
        catch
        {
            // A session can disappear between enumeration and property access.
        }

        return null;
    }

    /// Builds a stable, filesystem-safe name. The sequence prefix keeps album
    /// order even when two tracks share a title.
    public static string BuildFileName(TrackInfo track, int sequence)
    {
        string artist = track.PrimaryArtist;
        string stem = string.IsNullOrWhiteSpace(artist)
            ? track.Title
            : $"{artist} - {track.Title}";

        foreach (char invalid in Path.GetInvalidFileNameChars())
        {
            stem = stem.Replace(invalid, '_');
        }

        stem = stem.Trim().TrimEnd('.');
        if (stem.Length == 0) stem = "track";
        if (stem.Length > 110) stem = stem[..110];

        return $"{sequence:00} - {stem}.ec3";
    }
}

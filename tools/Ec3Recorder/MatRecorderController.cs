using System;
using System.IO;
using System.Threading;
using System.Threading.Tasks;

namespace Ec3Recorder;

/// <summary>
/// Drives the MAT recorder the same way the E-AC-3 UI drives <see cref="Ec3RecorderService"/>:
/// poll the non-destructive status probe, start the long-lived recorder only once a
/// real MAT carrier is flowing, then watch its progress to confirm valid MAT bursts
/// (and reject null/other carriers) and to detect stream stop / format change so the
/// current file is finalised cleanly.
///
/// At most one ring consumer exists at any time: the gate uses the read-only stats
/// probe, and once recording starts the controller watches the recorder's own STATUS
/// output instead of probing the ring again. This satisfies the constraint that the
/// status probe and the formal recording must never compete for the same ring buffer.
/// </summary>
internal sealed class MatRecorderController : IDisposable
{
    private readonly string _cliPath;
    private readonly MatRecorderService _service = new();
    private CancellationTokenSource? _cts;

    private long _lastCarrier;
    private long _lastFormatChanges;
    private long _lastValidBursts;

    public event Action<string>? StatusMessage;
    public event Action<MatRecordProgress>? Progress;
    public event Action<string>? FormatDetected;

    public MatRecorderController(string cliPath) => _cliPath = cliPath;

    public void Start(string outputDirectory)
    {
        Stop();
        _cts = new CancellationTokenSource();
        _ = RunAsync(outputDirectory, _cts.Token);
    }

    public void Stop()
    {
        _cts?.Cancel();
        _service.Stop();
        _cts = null;
    }

    private async Task RunAsync(string outputDirectory, CancellationToken token)
    {
        _service.Progress += OnProgress;
        _service.FormatDetected += f => FormatDetected?.Invoke(f);

        const int confirmMs = 2000;
        try
        {
            while (!token.IsCancellationRequested)
            {
                MatStreamStatus? status = await MatRecorderService.QueryStatus(_cliPath, token);
                if (status is null || !status.IsLive)
                {
                    StatusMessage?.Invoke(
                        $"GATE waiting (known={status?.Known}, mat={status?.IsMat}, flowing={status?.BytesPerProbe})");
                    await Task.Delay(500, token);
                    continue;
                }

                // A real MAT carrier is flowing: start recording a timestamped track file.
                Directory.CreateDirectory(outputDirectory);
                string currentFile = Path.Combine(outputDirectory, $"mat-{DateTime.Now:yyyyMMdd-HHmmss}.mlp");
                if (!_service.Start(_cliPath, currentFile))
                {
                    await Task.Delay(500, token);
                    continue;
                }
                StatusMessage?.Invoke($"RECORDING {currentFile}");

                // Confirm genuine MAT content (not a null/other carrier) within confirmMs.
                bool confirmed = await ConfirmMatContentAsync(confirmMs, token);
                if (!confirmed)
                {
                    _service.Stop();
                    StatusMessage?.Invoke("REJECTED null/other carrier, re-gating");
                    await Task.Delay(500, token);
                    continue;
                }

                // Monitor until the stream stops or the format changes.
                await MonitorAsync(token);
                _service.Stop();
                StatusMessage?.Invoke($"STOPPED {currentFile}");
            }
        }
        catch (OperationCanceledException)
        {
            // Expected on Stop().
        }
        finally
        {
            _service.Progress -= OnProgress;
        }
    }

    private void OnProgress(MatRecordProgress p)
    {
        Progress?.Invoke(p);
        _lastCarrier = p.CarrierBytes;
        _lastFormatChanges = p.FormatChanges;
        _lastValidBursts = p.ValidBursts;
    }

    /// <summary>
    /// Within <paramref name="confirmMs"/>, require at least one valid MAT burst to
    /// prove the carrier is real TrueHD rather than a NULL or other-type carrier.
    /// </summary>
    private async Task<bool> ConfirmMatContentAsync(int confirmMs, CancellationToken token)
    {
        _lastValidBursts = 0;
        using var cts = CancellationTokenSource.CreateLinkedTokenSource(token);
        cts.CancelAfter(confirmMs);
        try
        {
            while (!cts.Token.IsCancellationRequested && _service.IsRecording)
            {
                await Task.Delay(100, cts.Token);
            }
        }
        catch (OperationCanceledException)
        {
            // Timed out waiting for confirmation window; evaluate below.
        }
        return _lastValidBursts > 0;
    }

    /// <summary>
    /// Watches the recorder's own STATUS (carrier delta + formatChanges). The stream
    /// is considered ended when carrier bytes stop advancing for ~1.5s, or a format
    /// change is reported. Returns so the caller finalises the current file.
    /// </summary>
    private async Task MonitorAsync(CancellationToken token)
    {
        long lastCarrier = _lastCarrier;
        int stallCount = 0;
        while (!token.IsCancellationRequested && _service.IsRecording)
        {
            await Task.Delay(500, token);

            if (_lastFormatChanges > 0)
            {
                StatusMessage?.Invoke("FORMAT CHANGE detected");
                return;
            }

            if (_lastCarrier == lastCarrier)
            {
                ++stallCount;
            }
            else
            {
                stallCount = 0;
                lastCarrier = _lastCarrier;
            }

            if (stallCount >= 3)
            {
                StatusMessage?.Invoke("STREAM ENDED (no flow)");
                return;
            }
        }
    }

    public void Dispose() => Stop();
}
